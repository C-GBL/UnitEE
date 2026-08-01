using System;

namespace Ps2.Editor
{
    /// <summary>
    /// Thrown by any stage of <see cref="PS2BuildPipeline"/> and by the exporters.
    /// Plan section 7 requires the build to fail loudly with an actionable error
    /// rather than produce a broken ISO, so every unimplemented or failed stage
    /// raises this exception instead of logging and continuing.
    /// </summary>
    public sealed class PS2BuildException : Exception
    {
        public PS2BuildException(string message)
            : base(message)
        {
        }

        public PS2BuildException(string message, Exception innerException)
            : base(message, innerException)
        {
        }
    }
}
