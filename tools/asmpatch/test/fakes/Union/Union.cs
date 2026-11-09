// A stand-in for a game's enum <-> integer union, written for the mono5-generic-union test: a generic
// struct with an explicit layout, every field at offset 0, used only through locals (as compiled by
// C#: ldloca, initobj, stfld, ldfld), and a static constructor that calls it. .NET refuses the type
// as Mono 5 does ("generic types cannot have explicit layout"); the patched copy must give the union's
// own values: the same bytes written, the same bytes read, in 8 zeroed bytes.
using System;
using System.Runtime.InteropServices;

public enum Small : byte { A = 1, B = 200 }
public enum Normal { X = -5, Y = 123456 }
public enum Big : long { P = 1L << 40, Q = -3 }

public static class FastEnum
{
    [StructLayout(LayoutKind.Explicit)]
    struct Converter<T> where T : IConvertible
    {
        [FieldOffset(0)] public T Raw;
        [FieldOffset(0)] public sbyte AsSByte;
        [FieldOffset(0)] public byte AsByte;
        [FieldOffset(0)] public int AsInt;
        [FieldOffset(0)] public long AsLong;
        [FieldOffset(0)] public ulong AsULong;
    }

    public static int ToInt32<T>(T v) where T : IConvertible { var c = default(Converter<T>); c.Raw = v; return c.AsInt; }
    public static long ToLong<T>(T v) where T : IConvertible { var c = default(Converter<T>); c.Raw = v; return c.AsLong; }
    public static byte ToByte<T>(T v) where T : IConvertible { var c = default(Converter<T>); c.Raw = v; return c.AsByte; }
    public static T ToEnum<T>(int v) where T : IConvertible { var c = default(Converter<T>); c.AsInt = v; return c.Raw; }
    public static T ToEnumL<T>(long v) where T : IConvertible { var c = default(Converter<T>); c.AsLong = v; return c.Raw; }
}

public static class Keys
{
    public static readonly int Up = FastEnum.ToInt32(Normal.Y);
}
