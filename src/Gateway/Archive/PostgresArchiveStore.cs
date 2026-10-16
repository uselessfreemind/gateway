using System.Globalization;
using Gateway.Acquisition;
using Npgsql;
using NpgsqlTypes;

namespace Gateway.Archive;

// PostgreSQL is the scalable deployment option: binary COPY, daily partitions and partition expiry.
public sealed class PostgresArchiveStore(GatewayOptions options) : IArchiveStore
{
    private readonly HashSet<DateOnly> partitions = [];
    private NpgsqlConnection Open() { var c = new NpgsqlConnection(options.PostgresConnectionString); c.Open(); return c; }
    public void Initialize()
    {
        using var c = Open(); using var cmd = c.CreateCommand();
        cmd.CommandText = """
            CREATE SCHEMA IF NOT EXISTS scada;
            CREATE TABLE IF NOT EXISTS scada.samples (
                id BIGINT GENERATED ALWAYS AS IDENTITY, controller TEXT NOT NULL, tag TEXT NOT NULL,
                timestamp_ms BIGINT NOT NULL, value DOUBLE PRECISION, quality TEXT NOT NULL, error TEXT,
                PRIMARY KEY(timestamp_ms,id)) PARTITION BY RANGE(timestamp_ms);
            CREATE INDEX IF NOT EXISTS ix_samples_tag_time ON scada.samples(controller,tag,timestamp_ms,id);
            """;
        cmd.ExecuteNonQuery();
    }
    private void EnsurePartition(NpgsqlConnection c, DateOnly day)
    {
        if (partitions.Contains(day)) return;
        var start = new DateTimeOffset(day.ToDateTime(TimeOnly.MinValue, DateTimeKind.Utc));
        var name = "samples_p" + day.ToString("yyyyMMdd", CultureInfo.InvariantCulture);
        using var cmd = c.CreateCommand();
        // Identifier and bounds are generated solely from a parsed UTC date.
        cmd.CommandText = $"CREATE TABLE IF NOT EXISTS scada.{name} PARTITION OF scada.samples FOR VALUES FROM ({start.ToUnixTimeMilliseconds()}) TO ({start.AddDays(1).ToUnixTimeMilliseconds()})";
        cmd.ExecuteNonQuery(); partitions.Add(day);
    }
    public void Append(IReadOnlyList<Sample> samples)
    {
        using var c = Open();
        foreach (var day in samples.Select(s => DateOnly.FromDateTime(DateTimeOffset.FromUnixTimeMilliseconds(s.TimestampMs).UtcDateTime)).Distinct()) EnsurePartition(c, day);
        using var writer = c.BeginBinaryImport("COPY scada.samples(controller,tag,timestamp_ms,value,quality,error) FROM STDIN (FORMAT BINARY)");
        foreach (var s in samples)
        {
            writer.StartRow(); writer.Write(s.ControllerId, NpgsqlDbType.Text); writer.Write(s.TagId, NpgsqlDbType.Text);
            writer.Write(s.TimestampMs, NpgsqlDbType.Bigint);
            if (s.Value.HasValue) writer.Write(s.Value.Value, NpgsqlDbType.Double); else writer.WriteNull();
            writer.Write(s.Quality, NpgsqlDbType.Text);
            if (s.Error != null) writer.Write(s.Error, NpgsqlDbType.Text); else writer.WriteNull();
        }
        writer.Complete();
    }
    public long Purge(DateTimeOffset now)
    {
        using var c = Open();
        var cutoff = now.AddMonths(-1);
        var names = new List<string>();
        using (var list = c.CreateCommand())
        {
            list.CommandText = "SELECT child.relname FROM pg_inherits i JOIN pg_class parent ON i.inhparent=parent.oid JOIN pg_namespace ns ON parent.relnamespace=ns.oid JOIN pg_class child ON i.inhrelid=child.oid WHERE ns.nspname='scada' AND parent.relname='samples'";
            using var reader = list.ExecuteReader();
            while (reader.Read()) names.Add(reader.GetString(0));
        }
        foreach (var name in names)
        {
            if (!name.StartsWith("samples_p", StringComparison.Ordinal) || !DateOnly.TryParseExact(name[9..], "yyyyMMdd", CultureInfo.InvariantCulture, DateTimeStyles.None, out var day)) continue;
            var end = new DateTimeOffset(day.AddDays(1).ToDateTime(TimeOnly.MinValue, DateTimeKind.Utc));
            if (end > cutoff) continue;
            using var drop = c.CreateCommand(); drop.CommandText = $"DROP TABLE scada.{name}"; drop.ExecuteNonQuery();
        }
        using var cmd = c.CreateCommand();
        cmd.CommandText = "DELETE FROM scada.samples WHERE (tableoid,ctid) IN (SELECT tableoid,ctid FROM scada.samples WHERE timestamp_ms < $1 LIMIT 10000)";
        cmd.Parameters.AddWithValue(cutoff.ToUnixTimeMilliseconds());
        int deleted; long total = 0;
        do { deleted = cmd.ExecuteNonQuery(); total += deleted; } while (deleted == 10000);
        // Return rows individually deleted; dropped partitions are reported separately by PostgreSQL.
        return total;
    }
    public List<ArchivedSample> History(string controller, string tag, long from, long to, int limit, long afterId)
    {
        using var c = Open(); using var cmd = c.CreateCommand();
        cmd.CommandText = "SELECT id,controller,tag,timestamp_ms,value,quality,error FROM scada.samples WHERE controller=$1 AND tag=$2 AND timestamp_ms >= $3 AND timestamp_ms <= $4 AND id > $5 ORDER BY id LIMIT $6";
        AddRange(cmd, controller, tag, from, to); cmd.Parameters.AddWithValue(afterId); cmd.Parameters.AddWithValue(limit);
        using var r = cmd.ExecuteReader(); var result = new List<ArchivedSample>();
        while (r.Read()) result.Add(new(r.GetInt64(0), r.GetString(1), r.GetString(2), r.GetInt64(3), r.IsDBNull(4) ? null : r.GetDouble(4), r.GetString(5), r.IsDBNull(6) ? null : r.GetString(6)));
        return result;
    }
    public List<TrendBucket> Trend(string controller, string tag, long from, long to, long bucketMs)
    {
        using var c = Open(); using var cmd = c.CreateCommand();
        cmd.CommandText = "SELECT (timestamp_ms / $5) * $5 AS bucket, COUNT(*), COUNT(value), MIN(value), MAX(value), AVG(value) FROM scada.samples WHERE controller=$1 AND tag=$2 AND timestamp_ms >= $3 AND timestamp_ms <= $4 GROUP BY bucket ORDER BY bucket";
        AddRange(cmd, controller, tag, from, to); cmd.Parameters.AddWithValue(bucketMs);
        using var r = cmd.ExecuteReader(); var result = new List<TrendBucket>();
        while (r.Read()) result.Add(new(r.GetInt64(0), r.GetInt64(1), r.GetInt64(2), r.IsDBNull(3) ? null : r.GetDouble(3), r.IsDBNull(4) ? null : r.GetDouble(4), r.IsDBNull(5) ? null : r.GetDouble(5)));
        return result;
    }
    private static void AddRange(NpgsqlCommand cmd, string controller, string tag, long from, long to)
    {
        cmd.Parameters.AddWithValue(controller); cmd.Parameters.AddWithValue(tag);
        cmd.Parameters.AddWithValue(Math.Max(from, DateTimeOffset.UtcNow.AddMonths(-1).ToUnixTimeMilliseconds())); cmd.Parameters.AddWithValue(to);
    }
}
