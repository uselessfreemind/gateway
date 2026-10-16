using Gateway.Acquisition;
using Microsoft.Data.Sqlite;

namespace Gateway.Archive;

public sealed record ArchivedSample(long Id, string ControllerId, string TagId, long TimestampMs, double? Value, string Quality, string? Error);
public sealed record TrendBucket(long TimestampMs, long Count, long GoodCount, double? Min, double? Max, double? Average);
public interface IArchiveStore
{
    void Initialize();
    void Append(IReadOnlyList<Sample> samples);
    long Purge(DateTimeOffset now);
    List<ArchivedSample> History(string controller, string tag, long from, long to, int limit, long afterId);
    List<TrendBucket> Trend(string controller, string tag, long from, long to, long bucketMs);
}
public sealed class ArchiveStore : IArchiveStore
{
    private readonly string connectionString;
    public ArchiveStore(GatewayOptions options)
    {
        var path = Path.GetFullPath(options.DatabasePath);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        connectionString = new SqliteConnectionStringBuilder { DataSource = path, Pooling = true, DefaultTimeout = 30 }.ToString();
    }
    private SqliteConnection Open() { var c = new SqliteConnection(connectionString); c.Open(); return c; }
    public void Initialize()
    {
        using var c = Open(); using var cmd = c.CreateCommand();
        cmd.CommandText = """
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=NORMAL;
            CREATE TABLE IF NOT EXISTS samples (
                id INTEGER PRIMARY KEY, controller TEXT NOT NULL, tag TEXT NOT NULL,
                timestamp_ms INTEGER NOT NULL, value REAL, quality TEXT NOT NULL, error TEXT);
            CREATE INDEX IF NOT EXISTS ix_samples_tag_time ON samples(controller, tag, timestamp_ms, id);
            CREATE INDEX IF NOT EXISTS ix_samples_time ON samples(timestamp_ms);
            """;
        cmd.ExecuteNonQuery();
    }
    public void Append(IReadOnlyList<Sample> samples)
    {
        using var c = Open(); using var tx = c.BeginTransaction(); using var cmd = c.CreateCommand();
        cmd.Transaction = tx;
        cmd.CommandText = "INSERT INTO samples(controller,tag,timestamp_ms,value,quality,error) VALUES($c,$t,$time,$v,$q,$e)";
        foreach (var name in new[] { "$c", "$t", "$time", "$v", "$q", "$e" }) cmd.Parameters.Add(new SqliteParameter(name, null));
        foreach (var s in samples)
        {
            cmd.Parameters[0].Value = s.ControllerId; cmd.Parameters[1].Value = s.TagId; cmd.Parameters[2].Value = s.TimestampMs;
            cmd.Parameters[3].Value = (object?)s.Value ?? DBNull.Value; cmd.Parameters[4].Value = s.Quality;
            cmd.Parameters[5].Value = (object?)s.Error ?? DBNull.Value; cmd.ExecuteNonQuery();
        }
        tx.Commit();
    }
    public long Purge(DateTimeOffset now)
    {
        using var c = Open(); using var cmd = c.CreateCommand();
        cmd.CommandText = "DELETE FROM samples WHERE id IN (SELECT id FROM samples WHERE timestamp_ms < $cutoff LIMIT 10000)";
        cmd.Parameters.AddWithValue("$cutoff", now.AddMonths(-1).ToUnixTimeMilliseconds());
        long total = 0;
        int deleted;
        do { deleted = cmd.ExecuteNonQuery(); total += deleted; } while (deleted == 10000);
        return total;
    }
    public List<ArchivedSample> History(string controller, string tag, long from, long to, int limit, long afterId)
    {
        using var c = Open(); using var cmd = c.CreateCommand();
        cmd.CommandText = """
            SELECT id,controller,tag,timestamp_ms,value,quality,error FROM samples
            WHERE controller=$c AND tag=$t AND timestamp_ms >= $from AND timestamp_ms <= $to AND id > $after
            ORDER BY id LIMIT $limit
            """;
        AddRange(cmd, controller, tag, from, to);
        cmd.Parameters.AddWithValue("$after", afterId); cmd.Parameters.AddWithValue("$limit", limit);
        using var r = cmd.ExecuteReader(); var result = new List<ArchivedSample>();
        while (r.Read()) result.Add(new(r.GetInt64(0), r.GetString(1), r.GetString(2), r.GetInt64(3), r.IsDBNull(4) ? null : r.GetDouble(4), r.GetString(5), r.IsDBNull(6) ? null : r.GetString(6)));
        return result;
    }
    public List<TrendBucket> Trend(string controller, string tag, long from, long to, long bucketMs)
    {
        using var c = Open(); using var cmd = c.CreateCommand();
        cmd.CommandText = """
            SELECT (timestamp_ms / $bucket) * $bucket AS bucket, COUNT(*), COUNT(value), MIN(value), MAX(value), AVG(value)
            FROM samples WHERE controller=$c AND tag=$t AND timestamp_ms >= $from AND timestamp_ms <= $to
            GROUP BY bucket ORDER BY bucket
            """;
        AddRange(cmd, controller, tag, from, to); cmd.Parameters.AddWithValue("$bucket", bucketMs);
        using var r = cmd.ExecuteReader(); var result = new List<TrendBucket>();
        while (r.Read()) result.Add(new(r.GetInt64(0), r.GetInt64(1), r.GetInt64(2), r.IsDBNull(3) ? null : r.GetDouble(3), r.IsDBNull(4) ? null : r.GetDouble(4), r.IsDBNull(5) ? null : r.GetDouble(5)));
        return result;
    }
    private static void AddRange(SqliteCommand cmd, string controller, string tag, long from, long to)
    {
        cmd.Parameters.AddWithValue("$c", controller); cmd.Parameters.AddWithValue("$t", tag);
        cmd.Parameters.AddWithValue("$from", Math.Max(from, DateTimeOffset.UtcNow.AddMonths(-1).ToUnixTimeMilliseconds()));
        cmd.Parameters.AddWithValue("$to", to);
    }
}
