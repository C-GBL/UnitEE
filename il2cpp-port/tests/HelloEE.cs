using System;
using System.Collections.Generic;

public interface IShout { string Shout(); }

public class Animal : IShout
{
    private readonly string m_name;
    public Animal(string name) { m_name = name; }
    public virtual string Shout() { return m_name + "!"; }
}

public class Dog : Animal
{
    public Dog() : base("woof") {}
    public override string Shout() { return "WOOF"; }
}

public struct Pair<T>
{
    public T A, B;
    public Pair(T a, T b) { A = a; B = b; }
    public T First() { return A; }
}

public struct Vec3
{
    public float X, Y, Z;
    public Vec3(float x, float y, float z) { X = x; Y = y; Z = z; }
    public static Vec3 Add(Vec3 a, Vec3 b)
    {
        return new Vec3(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    }
}

public class AllocNode
{
    public int Value;
    public AllocNode Next;
    public AllocNode(int v) { Value = v; }
}

public static class HelloEE
{
    public delegate int Combiner(int a, int b);

    // Plan M6 gate measurements. Environment.TickCount is the COP0-backed
    // millisecond clock (os::Time::GetTicksMillisecondsMonotonic).
    private static void Bench()
    {
        // 1M Vector3 adds; budget <= 250 ms (plan section 15).
        var acc = new Vec3(0f, 0f, 0f);
        var step = new Vec3(1f, 2f, 3f);
        int t0 = Environment.TickCount;
        for (int i = 0; i < 1000000; i++)
            acc = Vec3.Add(acc, step);
        int v3Ms = Environment.TickCount - t0;
        Console.WriteLine(string.Format("M6_BENCH_V3_MS={0} (sum={1})", v3Ms, (int)acc.X));

        // Same arithmetic in float and in double: the doubles are software-
        // emulated on the EE and this measures exactly how much that costs.
        float fa = 1.0000001f, fs = 0f;
        t0 = Environment.TickCount;
        for (int i = 0; i < 1000000; i++)
            fs = fs * fa + 0.5f;
        int floatMs = Environment.TickCount - t0;

        double da = 1.0000001, ds = 0.0;
        t0 = Environment.TickCount;
        for (int i = 0; i < 1000000; i++)
            ds = ds * da + 0.5;
        int doubleMs = Environment.TickCount - t0;
        Console.WriteLine(string.Format("M6_BENCH_FLOAT_MS={0} M6_BENCH_DOUBLE_MS={1} (f={2} d={3})",
                                        floatMs, doubleMs, (int)fs, (int)ds));

        // 200k small-object allocations (list threading defeats any
        // escape-style optimization). Null GC: allocation cost only.
        t0 = Environment.TickCount;
        AllocNode head = null;
        for (int i = 0; i < 200000; i++) {
            var n = new AllocNode(i);
            n.Next = head;
            head = n;
        }
        int allocMs = Environment.TickCount - t0;
        Console.WriteLine(string.Format("M6_BENCH_ALLOC_MS={0} (head={1})", allocMs, head.Value));
    }

    // Plan M6 gate: a GC cycle over a 2 MB live heap must stay under 8 ms,
    // plus the pause distribution over 10,000 small allocations and the
    // heap high-water mark. DateTime.UtcNow.Ticks is the 100 ns COP0-backed
    // clock (Stopwatch would drag System.dll into the image); all
    // arithmetic in longs (doubles are software-emulated on the EE).
    private static void GcBench()
    {
        var live = new List<byte[]>();
        for (int i = 0; i < 2048; i++)
            live.Add(new byte[1024]); // 2 MB live, held across the bench

        long worstUs = 0, totalUs = 0;
        for (int i = 0; i < 10; i++) {
            long start = DateTime.UtcNow.Ticks;
            GC.Collect();
            long us = (DateTime.UtcNow.Ticks - start) / 10;
            totalUs += us;
            if (us > worstUs) worstUs = us;
        }
        Console.WriteLine(string.Format("M6_BENCH_GC_COLLECT_US worst={0} avg={1} (2MB live, 10 cycles)",
                                        worstUs, totalUs / 10));

        long worstAllocUs = 0;
        int over1Ms = 0;
        for (int i = 0; i < 10000; i++) {
            long start = DateTime.UtcNow.Ticks;
            var tmp = new byte[64];
            long us = (DateTime.UtcNow.Ticks - start) / 10;
            if (us > worstAllocUs) worstAllocUs = us;
            if (us > 1000) over1Ms++;
            tmp[0] = (byte)i;
        }
        Console.WriteLine(string.Format("M6_BENCH_GC_ALLOC10K worst_us={0} over1ms={1}",
                                        worstAllocUs, over1Ms));

        Console.WriteLine(string.Format("M6_BENCH_HEAP_BYTES={0}", GC.GetTotalMemory(false)));
        GC.KeepAlive(live);
    }

    public static int Main()
    {
        Console.WriteLine("hello from IL2CPP on the Emotion Engine");

        // Generics + List + Dictionary.
        var list = new List<int>();
        for (int i = 1; i <= 10; i++) list.Add(i * i);
        int sum = 0;
        foreach (int v in list) sum += v;   // 385

        var dict = new Dictionary<string, int>();
        dict["alpha"] = 1;
        dict["beta"] = 2;
        dict["gamma"] = 3;
        int beta;
        bool found = dict.TryGetValue("beta", out beta);

        // Interfaces + virtual dispatch.
        IShout a = new Animal("meow");
        IShout d = new Dog();
        string shouts = a.Shout() + " " + d.Shout();

        // Delegates.
        Combiner add = (x, y) => x + y;
        Combiner mul = (x, y) => x * y;
        int combined = add(3, 4) + mul(3, 4); // 19

        // Generic struct.
        var pair = new Pair<int>(11, 22);

        // try/catch/finally.
        int caught = 0;
        int finallyRan = 0;
        try {
            try {
                throw new InvalidOperationException("managed throw");
            } finally {
                finallyRan = 1;
            }
        } catch (InvalidOperationException e) {
            caught = e.Message.Length; // 13
        }

        // string work.
        string s = string.Format("sum={0} beta={1} shouts={2} comb={3} pair={4} catch={5}/{6}",
                                 sum, found ? beta : -1, shouts, combined,
                                 pair.First(), caught, finallyRan);
        Console.WriteLine(s);

        bool ok = sum == 385 && found && beta == 2 && shouts == "meow! WOOF" &&
                  combined == 19 && pair.First() == 11 && caught == 13 &&
                  finallyRan == 1;
        Console.WriteLine(ok ? "M6_GATE_FEATURES_OK" : "M6_GATE_FEATURES_FAIL");

        Bench();
        GcBench();
        return ok ? 0 : 1;
    }
}
