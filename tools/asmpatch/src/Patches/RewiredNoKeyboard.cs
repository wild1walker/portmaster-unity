// rewired-no-keyboard (opt-in): Rewired's "Disable Keyboard" platform option on, as a game's Android build
// can set it. Unity's Android player reports some gamepad buttons as keyboard keys as well as joystick
// buttons (Unity 2017.4, measured on ua_host with Input.GetKey: L1 -> LeftShift + joystick button 4,
// R1 -> RightShift + button 5, L2 -> LeftAlt, R2 -> RightAlt, Start -> Return + button 10). A desktop
// game's pad never produced those keys. Rewired polls the keyboard through Unity's Input.GetKey, so a game
// that follows the last active controller (its prompts' glyphs, Blasphemous's InputManager) switches to the
// keyboard at every press of those buttons, and actions the keyboard map binds to Shift, Alt or Return
// fire too. With the option on Rewired leaves its keyboard controller disabled (its own
// "enabled = !ConfigVars.GetPlatformVar_disableKeyboard()") and polls no keys.
//
// The patch makes ConfigVars.GetPlatformVar_disableKeyboard return true (it is read when Rewired starts).
// Opt-in: for a port whose players have no keyboard (ua_host's UA_KEYMAP, UA_KEYBOARD and the on-screen
// keyboard's keys reach Unity as keyboard keys and would then be ignored by Rewired too).
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace AsmPatch;

sealed class RewiredNoKeyboard : Patch
{
    public override string Name => "rewired-no-keyboard";
    public override bool OptIn => true;
    public override string Summary =>
        "Rewired's Disable Keyboard platform option on: the pad's shoulder, trigger and Start buttons, which Unity's Android player also reports as Shift, Alt and Return, no longer make the keyboard the active controller";

    public override Result Run(Game game, bool apply)
    {
        var targets = new List<(AssemblyDefinition asm, MethodDefinition m)>();
        foreach (var asm in game.GameAssemblies)
            foreach (var t in Cecil.AllTypes(asm.MainModule))
            {
                if (t.FullName != "Rewired.Data.ConfigVars") continue;
                var m = t.Methods.FirstOrDefault(x => x.Name == "GetPlatformVar_disableKeyboard" && !x.IsStatic && x.HasBody &&
                                                      x.Parameters.Count == 0 && x.ReturnType.MetadataType == MetadataType.Boolean);
                if (m != null) targets.Add((asm, m));
            }
        if (targets.Count == 0) return Result.NotApplicable("no Rewired.Data.ConfigVars.GetPlatformVar_disableKeyboard");
        var r = new Result { Outcome = Outcome.AlreadyApplied };
        foreach (var (asm, m) in targets)
        {
            var where = $"{game.FileOf(asm)}: Rewired.Data.ConfigVars.GetPlatformVar_disableKeyboard";
            var ins = m.Body.Instructions;
            if (ins.Count >= 2 && ins[0].OpCode.Code == Code.Ldc_I4_1 && ins[1].OpCode.Code == Code.Ret)
            {
                r.Details.Add($"{where}: already returns true");
                continue;
            }
            r.Outcome = Outcome.Applied;
            r.Details.Add($"{where}: returns true (the keyboard controller disabled)");
            if (!apply) continue;
            var il = m.Body.GetILProcessor();
            var first = ins[0];
            il.InsertBefore(first, il.Create(OpCodes.Ldc_I4_1));
            il.InsertBefore(first, il.Create(OpCodes.Ret));
            game.MarkChanged(asm);
        }
        return r;
    }
}
