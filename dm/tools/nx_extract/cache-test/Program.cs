// LibHac 0.19 builds an integrity level's cache with min(blocks, 4) entries, so a cache is empty only when its level is empty; such a level
// is never read through its cache (any read past 0 bytes fails the range check first). nx_extract therefore needs no cache repair, and it
// must not use reflection on LibHac internals: the NativeAOT build drops that metadata (setup failed with "LibHac cache count was not found").
using System;
using System.Linq;
using System.Reflection;
using LibHac.Fs;
using LibHac.Tools.FsSystem;

var empty = new CachedStorage(new MemoryStorage(Array.Empty<byte>()), 4, 0, true);
empty.Read(0, Span<byte>.Empty).ThrowIfFailure();
if (empty.Read(0, new byte[1]).IsSuccess()) throw new Exception("A read past an empty cached level succeeded");

var one = new CachedStorage(new MemoryStorage(new byte[] { 5, 6, 7, 8 }), 4, 1, true);
byte[] got = new byte[4];
one.Read(0, got).ThrowIfFailure();
if (!got.SequenceEqual(new byte[] { 5, 6, 7, 8 })) throw new Exception("One-entry cache returned incorrect data");

Type p = Assembly.Load("nx_extract").GetType("P") ?? throw new Exception("nx_extract's P was not found");
string[] repair = p.GetMethods(BindingFlags.Static | BindingFlags.NonPublic).Select(m => m.Name).Where(n => n.Contains("Cache")).ToArray();
if (repair.Length != 0) throw new Exception($"nx_extract still reaches into LibHac's cache: {string.Join(", ", repair)}");

Console.WriteLine("Empty cached levels are never read; nx_extract uses no LibHac cache reflection.");
