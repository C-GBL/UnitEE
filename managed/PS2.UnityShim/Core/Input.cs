using UnityEngine.Internal;

namespace UnityEngine
{
    // The DualShock 2 buttons, in the native layer's order.
    public enum PS2Button
    {
        Select = 0,
        L3,
        R3,
        Start,
        Up,
        Right,
        Down,
        Left,
        L2,
        R2,
        L1,
        R1,
        Triangle,
        Circle,
        Cross,
        Square,
    }

    // Unity-shaped input (plan section 7.1, M10 task 2).
    //
    // SUBSET DISCIPLINE (ADR-007). Unity's Input is enormous and most of it
    // describes hardware a PS2 does not have. What exists here behaves
    // exactly as Unity's does:
    //
    //   GetAxis / GetAxisRaw  -- "Horizontal", "Vertical", "Mouse X" style
    //                            names mapped to the sticks. Both return the
    //                            same value: there is no smoothing filter to
    //                            differ from, and inventing one would make
    //                            GetAxisRaw a lie.
    //   GetButton / Down / Up -- "Fire1".."Fire3", "Jump", "Submit",
    //                            "Cancel", mapped to the standard PS2 face
    //                            buttons.
    //
    // Absent on purpose: mousePosition, touches, GetKey (there is no
    // keyboard), acceleration, and the Input Manager's configurable axes --
    // a PS2 build has no inspector to configure them in. PS2Button-level
    // access, pressure and rumble live on PS2Input below.
    public static class Input
    {
        // Unity's default axis names. A name this does not know returns 0,
        // exactly as Unity does for an unconfigured axis.
        public static float GetAxis(string axisName) => GetAxisRaw(axisName);

        public static float GetAxisRaw(string axisName)
        {
            switch (axisName)
            {
                case "Horizontal":
                    return Native.ps2ur_input_axis(0, 0, 0);
                case "Vertical":
                    return Native.ps2ur_input_axis(0, 0, 1);
                // The right stick is where a PS2 game puts the camera, which
                // is what Unity projects map to Mouse X/Y.
                case "Mouse X":
                case "Horizontal2":
                    return Native.ps2ur_input_axis(0, 1, 0);
                case "Mouse Y":
                case "Vertical2":
                    return Native.ps2ur_input_axis(0, 1, 1);
                default:
                    return 0f;
            }
        }

        public static bool GetButton(string buttonName)
        {
            int button = MapButton(buttonName);
            return button >= 0 && Native.ps2ur_input_button(0, button) != 0;
        }

        public static bool GetButtonDown(string buttonName)
        {
            int button = MapButton(buttonName);
            return button >= 0 && Native.ps2ur_input_button_down(0, button) != 0;
        }

        public static bool GetButtonUp(string buttonName)
        {
            int button = MapButton(buttonName);
            return button >= 0 && Native.ps2ur_input_button_up(0, button) != 0;
        }

        private static int MapButton(string name)
        {
            switch (name)
            {
                case "Fire1":
                case "Submit":
                case "Jump":
                    return (int)PS2Button.Cross;
                case "Fire2":
                case "Cancel":
                    return (int)PS2Button.Circle;
                case "Fire3":
                    return (int)PS2Button.Square;
                case "Start":
                case "Pause":
                    return (int)PS2Button.Start;
                default:
                    return -1;
            }
        }
    }

    // The parts of a DualShock 2 that Unity's Input has no vocabulary for
    // (plan M10 task 2: "plus a PS2Input API exposing pressure and rumble
    // directly"). Port 0 is the first controller.
    public static class PS2Input
    {
        public static bool IsConnected(int port) =>
            Native.ps2ur_input_connected(port) != 0;

        public static bool GetButton(int port, PS2Button button) =>
            Native.ps2ur_input_button(port, (int)button) != 0;

        public static bool GetButtonDown(int port, PS2Button button) =>
            Native.ps2ur_input_button_down(port, (int)button) != 0;

        public static bool GetButtonUp(int port, PS2Button button) =>
            Native.ps2ur_input_button_up(port, (int)button) != 0;

        // Stick axes in -1..1, deadzone applied, Y positive up.
        public static float GetLeftStickX(int port) =>
            Native.ps2ur_input_axis(port, 0, 0);
        public static float GetLeftStickY(int port) =>
            Native.ps2ur_input_axis(port, 0, 1);
        public static float GetRightStickX(int port) =>
            Native.ps2ur_input_axis(port, 1, 0);
        public static float GetRightStickY(int port) =>
            Native.ps2ur_input_axis(port, 1, 1);

        // How hard a button is pressed, 0..255. Returns 0 on a pad that does
        // not report pressure -- and note that a digital press reads 255,
        // not 0, so a game can treat this as "at least held".
        public static int GetPressure(int port, PS2Button button) =>
            Native.ps2ur_input_pressure(port, (int)button);

        // The small motor is on or off; the large one takes 0..255.
        public static void SetRumble(int port, bool smallMotor, int largeMotor)
        {
            Native.ps2ur_input_set_rumble(port, smallMotor ? 1 : 0, largeMotor);
        }
    }
}
