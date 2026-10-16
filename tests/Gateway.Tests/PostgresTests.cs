using Gateway.Acquisition;
using Gateway.Archive;
using Npgsql;
using Xunit;

namespace Gateway.Tests;

public sealed class PostgresFactAttribute : FactAttribute
{
    public PostgresFactAttribute()
    {
        if (string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("GATEWAY_TEST_POSTGRES")))
            Skip = "Optional PostgreSQL integration: set GATEWAY_TEST_POSTGRES to an isolated test database connection string.";
    }
}
public class PostgresTests
{
    [PostgresFact]
    public void CopyHistoryTrendsAndPartitionRetention()
    {
        var connectionString = Environment.GetEnvironmentVariable("GATEWAY_TEST_POSTGRES")!;
        var store = new PostgresArchiveStore(new() { PostgresConnectionString = connectionString }); store.Initialize(); store.Initialize();
        var id = "test-" + Guid.NewGuid().ToString("N");
        var now = DateTimeOffset.UtcNow; var t = now.ToUnixTimeMilliseconds();
        store.Append([new(id, "t", 1, now.AddMonths(-2).ToUnixTimeMilliseconds(), 1, "Good", null),
            new(id, "t", 1, now.AddMonths(-1).AddMinutes(-1).ToUnixTimeMilliseconds(), 2, "Good", null),
            new(id, "t", 1, t, 10, "Good", null), new(id, "t", 1, t + 1, 20, "Good", null), new(id, "t", 1, t + 2, null, "Bad", "timeout")]);
        var rows = store.History(id, "t", 0, t + 1000, 2, 0); Assert.Equal(2, rows.Count);
        Assert.Equal(10d, rows[0].Value); Assert.Single(store.History(id, "t", 0, t + 1000, 100, rows[^1].Id));
        var trend = store.Trend(id, "t", t, t + 1000, 1000000);
        Assert.Equal(3, trend.Sum(x => x.Count)); Assert.Equal(2, trend.Sum(x => x.GoodCount));
        Assert.Equal(10d, trend.Min(x => x.Min)); Assert.Equal(20d, trend.Max(x => x.Max));
        store.Purge(now); Assert.Equal(0, store.Purge(now));
        using var c = new NpgsqlConnection(connectionString); c.Open(); using var cmd = c.CreateCommand();
        cmd.CommandText = "SELECT COUNT(*) FROM scada.samples WHERE controller=$1"; cmd.Parameters.AddWithValue(id);
        Assert.Equal(3L, (long)cmd.ExecuteScalar()!);
    }
}
