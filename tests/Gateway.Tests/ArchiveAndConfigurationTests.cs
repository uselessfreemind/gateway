using Gateway.Acquisition;
using Gateway.Archive;
using Gateway.Protocols;
using Xunit;

namespace Gateway.Tests;

public class ArchiveAndConfigurationTests
{
    [Fact]
    public void ArchivePersistsPagesTrendsBadQualityAndCalendarMonthExpiry()
    {
        var path = Path.Combine(Path.GetTempPath(), $"gateway-test-{Guid.NewGuid()}.db");
        var options = new GatewayOptions { DatabasePath = path };
        var now = DateTimeOffset.UtcNow;
        var old = now.AddMonths(-1).AddSeconds(-1).ToUnixTimeMilliseconds();
        var t = now.ToUnixTimeMilliseconds();
        try
        {
            var store = new ArchiveStore(options); store.Initialize(); store.Initialize();
            store.Append([new("c", "t", 1, old, 9, "Good", null), new("c", "t", 1, t, 10, "Good", null), new("c", "t", 1, t + 1, 20, "Good", null), new("c", "t", 1, t + 2, null, "Bad", "timeout")]);
            var fresh = new ArchiveStore(options); // Independent store sees committed, durable rows.
            var page = fresh.History("c", "t", 0, t + 10, 2, 0);
            Assert.Equal(2, page.Count); Assert.Equal(10d, page[0].Value);
            var next = fresh.History("c", "t", 0, t + 10, 2, page[^1].Id);
            Assert.Single(next); Assert.Null(next[0].Value); Assert.Equal("Bad", next[0].Quality);
            var trend = fresh.Trend("c", "t", t, t + 10, 1000000);
            Assert.Equal(3, trend.Sum(x => x.Count)); Assert.Equal(2, trend.Sum(x => x.GoodCount));
            Assert.Equal(10d, trend.Min(x => x.Min)); Assert.Equal(20d, trend.Max(x => x.Max));
            Assert.Equal(1, fresh.Purge(now)); Assert.Equal(0, fresh.Purge(now));
            using var connection = new Microsoft.Data.Sqlite.SqliteConnection($"Data Source={path}"); connection.Open();
            using var count = connection.CreateCommand(); count.CommandText = "SELECT COUNT(*) FROM samples";
            Assert.Equal(3L, (long)count.ExecuteScalar()!);
        }
        finally { Microsoft.Data.Sqlite.SqliteConnection.ClearAllPools(); foreach (var suffix in new[] { "", "-wal", "-shm" }) File.Delete(path + suffix); }
    }
    [Fact]
    public void ConfigurationAccepts500ControllersWith700TagsAndRejects501()
    {
        var options = new GatewayOptions();
        for (var c = 0; c < 500; c++)
            options.Controllers.Add(new() { Id = $"plc{c}", Tags = Enumerable.Range(0, 700).Select(t => new TagOptions { Id = $"t{t}", Svid = (uint)(1 + c * 700 + t), Address = $"D:{t}" }).ToList() });
        options.Validate();
        Assert.Equal(1000, options.Controllers.Sum(c => ReadPlan.Create(c).Count));
        options.Controllers.Add(new() { Id = "extra" });
        Assert.Throws<ArgumentException>(options.Validate);
    }
    [Fact]
    public void DuplicateSvidIsRejected()
    {
        var options = new GatewayOptions { Controllers = [new() { Id = "c", Tags = [new() { Id = "a", Address = "D:0", Svid = 1 }, new() { Id = "b", Address = "D:1", Svid = 1 }] }] };
        Assert.Throws<ArgumentException>(options.Validate);
    }
    [Fact]
    public async Task QueueAppliesBackpressureInsteadOfDropping()
    {
        var queue = new SampleQueue(new() { QueueCapacity = 100 });
        var sample = new Sample("c", "t", 1, 0, 1, "Good", null);
        for (var i = 0; i < 100; i++) Assert.True(queue.Channel.Writer.TryWrite(sample));
        Assert.False(queue.Channel.Writer.TryWrite(sample));
        var pending = queue.Channel.Writer.WriteAsync(sample).AsTask(); Assert.False(pending.IsCompleted);
        await queue.Channel.Reader.ReadAsync(); await pending.WaitAsync(TimeSpan.FromSeconds(1));
    }
}
