using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
public class Base {
    public static StringBuilder Log = new StringBuilder();
    public int n;
    public string s;
    public virtual void V() { Log.Append("Base.V ").Append(n); }
    public void Ok() { n++; Log.Append("ok ").Append(n).Append(GetType().Name); }
    public void Throw() { Log.Append("t "); throw new InvalidOperationException("thrown " + n); }
    [MethodImpl(MethodImplOptions.NoInlining)] void Deep(int d) { if (d == 0) throw new ArgumentException("deep"); Deep(d - 1); }
    public void ThrowDeep() { Deep(5); }
    public void NullRef() { Log.Append("nr "); Log.Append(s.Length); }
    public void DivZero() { int z = n - n; Log.Append(10 / z); }
    public void Finally() { try { Log.Append("try "); throw new Exception("in try"); } finally { Log.Append("finally "); } }
    public void ThrowInFinally() { try { throw new Exception("first"); } finally { throw new FormatException("second"); } }
    public void Caught() { try { throw new Exception("x"); } catch (Exception e) { Log.Append("caught ").Append(e.Message); } }
    public void Rethrow() { try { Deep(2); } catch (Exception) { Log.Append("re "); throw; } }
    public void Wrapped() { try { Deep(1); } catch (Exception e) { throw new ApplicationException("wrap", e); } }
    public void Cast() { object o = "str"; Log.Append(((List<int>)o).Count); }
    public void Index() { int[] a = new int[2]; Log.Append(a[n + 5]); }
    public void Alloc() { var l = new List<string>(); for (int i = 0; i < 1000; i++) l.Add(i.ToString()); Log.Append(l.Count); GC.Collect(); Log.Append(" gc"); }
    public void Generic() { var g = new Gen<string>(); g.v = "gv"; Log.Append(g.Get()); }
    public void Recurse() { Recurse(); }
    public void ThrowNull() { throw null; }
    public void TypeInit() { Log.Append(Bad.X); }
    public bool Next() { n++; Log.Append("next ").Append(n); return n < 2; }
    public static void Stat() { Log.Append("static"); }
    public void Args(int a, string b) { Log.Append("args ").Append(a).Append(b); }
    public float Ret() { return 1.5f + n; }
}
public class Derived : Base {
    public override void V() { Log.Append("Derived.V ").Append(n); }
}
public class Gen<T> { public T v; public T Get() { return v; } public void G() { Base.Log.Append("Gen.G ").Append(typeof(T).Name); } }
public class Bad { public static int X = Init(); static int Init() { throw new Exception("cctor"); } }
public struct Val { public int a; public void M() { a++; Base.Log.Append("val ").Append(a); } }
public static class Report {
    public static string Get() { string r = Base.Log.ToString(); Base.Log.Length = 0; return r; }
    public static string Desc(Exception e) { return e == null ? "none" : e.GetType().FullName + "|" + e.Message + "|" + e.StackTrace + "|inner=" + (e.InnerException == null ? "" : e.InnerException.ToString()); }
}
