using System;
using System.Reflection;
using LibHac.Fs;
using LibHac.Tools.FsSystem;

var repair = Assembly.Load("nx_extract").GetType("P")?
    .GetMethod("EnsureNonEmptyIntegrityCaches", BindingFlags.Static | BindingFlags.NonPublic)
    ?? throw new Exception("Cache repair method was not found");
var cache = new CachedStorage(new MemoryStorage(new byte[] { 1, 2, 3, 4 }), 4, 0, true);
repair.Invoke(null, new object[] { cache });
byte[] actual = new byte[4];
cache.Read(0, actual).ThrowIfFailure();
if (!actual.AsSpan().SequenceEqual(new byte[] { 1, 2, 3, 4 }))
    throw new Exception("Cache repair returned incorrect data");

var nonEmpty = new CachedStorage(new MemoryStorage(new byte[] { 5, 6, 7, 8 }), 4, 1, true);
repair.Invoke(null, new object[] { nonEmpty });
byte[] existing = new byte[4];
nonEmpty.Read(0, existing).ThrowIfFailure();
if (!existing.AsSpan().SequenceEqual(new byte[] { 5, 6, 7, 8 }))
    throw new Exception("Cache repair changed the existing cache behavior");

Console.WriteLine("Zero-capacity cache read succeeds; one-entry cache behavior is unchanged.");
