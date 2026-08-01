namespace UnityEngine
{
    // UnityEngine.Random surface over a xorshift128 generator. Deliberately
    // NOT Unity's exact sequence (Unity's generator is unspecified); games
    // must not depend on cross-engine reproducibility, only on determinism
    // for a given seed on a given build, which xorshift128 provides on both
    // the Editor (x64) and the EE - it is pure 32-bit integer math.
    public static class Random
    {
        private static uint s_X, s_Y, s_Z, s_W;

        static Random()
        {
            InitState(System.Environment.TickCount);
        }

        public static void InitState(int seed)
        {
            uint s = (uint)seed;
            s_X = s != 0u ? s : 0x9E3779B9u;
            s_Y = s_X * 1812433253u + 1u;
            s_Z = s_Y * 1812433253u + 1u;
            s_W = s_Z * 1812433253u + 1u;
        }

        private static uint NextUInt()
        {
            uint t = s_X ^ (s_X << 11);
            s_X = s_Y; s_Y = s_Z; s_Z = s_W;
            s_W = s_W ^ (s_W >> 19) ^ (t ^ (t >> 8));
            return s_W;
        }

        // [0, 1] inclusive, 24-bit resolution.
        public static float value => (NextUInt() >> 8) * (1f / 16777215f);

        // [minInclusive, maxExclusive). Modulo bias is accepted (matches the
        // precision class of Unity's own implementation).
        public static int Range(int minInclusive, int maxExclusive)
        {
            if (maxExclusive <= minInclusive) return minInclusive;
            uint span = (uint)((long)maxExclusive - minInclusive);
            return (int)(minInclusive + (long)(NextUInt() % span));
        }

        // [minInclusive, maxInclusive].
        public static float Range(float minInclusive, float maxInclusive) =>
            minInclusive + value * (maxInclusive - minInclusive);

        public static Vector2 insideUnitCircle
        {
            get
            {
                // Rejection sampling; acceptance ~ pi/4.
                Vector2 v;
                do
                {
                    v = new Vector2(Range(-1f, 1f), Range(-1f, 1f));
                } while (v.sqrMagnitude > 1f);
                return v;
            }
        }

        public static Vector3 insideUnitSphere
        {
            get
            {
                // Rejection sampling; acceptance ~ pi/6.
                Vector3 v;
                do
                {
                    v = new Vector3(Range(-1f, 1f), Range(-1f, 1f), Range(-1f, 1f));
                } while (v.sqrMagnitude > 1f);
                return v;
            }
        }

        public static Vector3 onUnitSphere
        {
            get
            {
                Vector3 v;
                float sq;
                do
                {
                    v = new Vector3(Range(-1f, 1f), Range(-1f, 1f), Range(-1f, 1f));
                    sq = v.sqrMagnitude;
                } while (sq > 1f || sq < 1e-6f);
                return v / Mathf.Sqrt(sq);
            }
        }

        // Stub-level implementation: uniform over SO(3) by rejection-sampling
        // a point in the unit 4-ball and normalising onto S^3.
        public static Quaternion rotation
        {
            get
            {
                float x, y, z, w, sq;
                do
                {
                    x = Range(-1f, 1f);
                    y = Range(-1f, 1f);
                    z = Range(-1f, 1f);
                    w = Range(-1f, 1f);
                    sq = x * x + y * y + z * z + w * w;
                } while (sq > 1f || sq < 1e-6f);
                float inv = 1f / Mathf.Sqrt(sq);
                return new Quaternion(x * inv, y * inv, z * inv, w * inv);
            }
        }
    }
}
