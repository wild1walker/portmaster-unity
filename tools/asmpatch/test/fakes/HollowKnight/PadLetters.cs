// Stand-ins for the parts of Hollow Knight's input UI that hk-pad-letters looks at (written for the
// tests; no game code): the pad type the InputHandler keeps, the button glyph table chosen by it (the
// PlayStation pads first, then the Switch pads tested from a local, else the Xbox glyphs) and the
// controller picture shown for it. Each glyph is the name of the sprite the table would pick.
namespace GlobalEnums
{
    public enum GamepadType
    {
        NONE, UNKNOWN, XBOX_ONE, XBOX_360, PS4, PS_VITA, WII_U_GAMEPAD, WII_U_PRO_CONTROLLER, PS3_WIN,
        SWITCH_JOYCON_DUAL, SWITCH_PRO_CONTROLLER, PS5, XBOX_SERIES_X, STEAM_DECK, SWITCH2_JOYCON_DUAL, SWITCH2_PRO_CONTROLLER
    }
}

namespace InControl
{
    public enum InputControlType { None, Action1 = 19, Action2, Action3, Action4, LeftBumper, RightBumper, Start = 106, Select }
}

public class InputHandler
{
    public GlobalEnums.GamepadType activeGamepadType;
}

public class UIButtonSkins
{
    public InputHandler ih = new InputHandler();

    public string GetButtonSkinFor(InControl.InputControlType inputControlType)
    {
        string sprite = "blank";
        if (ih.activeGamepadType == GlobalEnums.GamepadType.PS4)
        {
            switch (inputControlType)
            {
                case InControl.InputControlType.Action1: sprite = "ps4x"; break;
                case InControl.InputControlType.Action2: sprite = "ps4circle"; break;
            }
        }
        else if (ih.activeGamepadType == GlobalEnums.GamepadType.PS5)
        {
            switch (inputControlType)
            {
                case InControl.InputControlType.Action1: sprite = "ps5cross"; break;
                case InControl.InputControlType.Action2: sprite = "ps5circle"; break;
            }
        }
        else
        {
            GlobalEnums.GamepadType activeGamepadType = ih.activeGamepadType;
            if (activeGamepadType == GlobalEnums.GamepadType.SWITCH_JOYCON_DUAL || activeGamepadType == GlobalEnums.GamepadType.SWITCH_PRO_CONTROLLER ||
                activeGamepadType == GlobalEnums.GamepadType.SWITCH2_JOYCON_DUAL || activeGamepadType == GlobalEnums.GamepadType.SWITCH2_PRO_CONTROLLER)
            {
                switch (inputControlType)
                {
                    case InControl.InputControlType.Action1: sprite = "switchHidB"; break;
                    case InControl.InputControlType.Action2: sprite = "switchHidA"; break;
                    case InControl.InputControlType.Action3: sprite = "switchHidY"; break;
                    case InControl.InputControlType.Action4: sprite = "switchHidX"; break;
                    case InControl.InputControlType.Start: sprite = "switchHidPlus"; break;
                    case InControl.InputControlType.Select: sprite = "switchHidMinus"; break;
                }
            }
            else
            {
                switch (inputControlType)
                {
                    case InControl.InputControlType.Action1: sprite = "a"; break;
                    case InControl.InputControlType.Action2: sprite = "b"; break;
                    case InControl.InputControlType.Action3: sprite = "x"; break;
                    case InControl.InputControlType.Action4: sprite = "y"; break;
                    case InControl.InputControlType.Start: sprite = "start"; break;
                    case InControl.InputControlType.Select: sprite = "select"; break;
                }
            }
        }
        return sprite;
    }
}

public class ControllerDetect
{
    public InputHandler ih = new InputHandler();
    public string Shown = "nothing";

    public void LookForActiveController()
    {
        if (ih.activeGamepadType == GlobalEnums.GamepadType.NONE)
            Shown = "prompt";
        else if (ih.activeGamepadType != GlobalEnums.GamepadType.NONE)
            ShowController(ih.activeGamepadType);
    }

    private void ShowController(GlobalEnums.GamepadType gamepadType)
    {
        Shown = gamepadType.ToString();
    }
}
