using System.Runtime.CompilerServices;

// Internal surface (Time.Sync, Native, ...) is shared with our own assemblies
// only. The play-mode stand-in name is fixed by the asmdef at
// unity-package/com.example.ps2/Runtime/com.example.ps2.Runtime.asmdef; keep
// these in sync if that asmdef is ever renamed.
[assembly: InternalsVisibleTo("PS2.UnityShim.Editor")]
[assembly: InternalsVisibleTo("PS2.Conformance")]
[assembly: InternalsVisibleTo("com.example.ps2.Runtime")]
