using System;

namespace DSPAAMod.Core
{
    public enum AaChoice { None, Msaa, Fxaa, Taa, Dlss }

    // Presentation state maps a single technique plus its configuration to the
    // plugin policy and the game's two legacy AA fields. It has no Unity dependency.
    public sealed class AaMenuDraft
    {
        public AaChoice Choice { get; private set; }
        public AaSettings Settings { get; private set; }
        public int NativeMsaa { get; private set; }
        public bool NativeFxaa { get; private set; }
        public int MsaaSamples { get; private set; }
        public bool ResolutionEnabled => Choice == AaChoice.Dlss;
        public bool ConfigurationEnabled => Choice == AaChoice.Msaa || Choice == AaChoice.Dlss;
        public int ConfigurationIndex => Choice == AaChoice.Dlss ? (int)Settings.Model :
            Choice == AaChoice.Msaa ? MsaaSamples == 8 ? 2 : MsaaSamples == 4 ? 1 : 0 : 0;

        public AaMenuDraft(AaSettings settings, int nativeMsaa, bool nativeFxaa)
        {
            Settings = settings;
            NativeMsaa = nativeMsaa == 2 || nativeMsaa == 4 || nativeMsaa == 8 ? nativeMsaa : 0;
            NativeFxaa = nativeFxaa;
            MsaaSamples = NativeMsaa == 0 ? 2 : NativeMsaa;
            Choice = settings.Technique == AaTechnique.Dlss ? AaChoice.Dlss :
                settings.Technique == AaTechnique.Taa ? AaChoice.Taa :
                settings.Technique == AaTechnique.Fxaa ? AaChoice.Fxaa :
                NativeMsaa > 0 ? AaChoice.Msaa : nativeFxaa ? AaChoice.Fxaa : AaChoice.None;
            // Merely opening the menu must not migrate an existing original-game
            // MSAA+FXAA combination. Explicit AA edits select one technique.
            if (settings.Technique != AaTechnique.Original) UpdateNative();
        }
        public bool TrySelectTechnique(AaChoice choice, DlssAvailability availability)
        {
            if (availability == null) throw new ArgumentNullException(nameof(availability));
            if (!availability.CanSelect(choice)) return false;
            SelectTechnique(choice);
            return true;
        }
        public void SelectTechnique(AaChoice choice)
        {
            if (!Enum.IsDefined(typeof(AaChoice), choice)) throw new ArgumentOutOfRangeException(nameof(choice));
            Choice = choice;
            AaTechnique technique = choice == AaChoice.Dlss ? AaTechnique.Dlss :
                choice == AaChoice.Taa ? AaTechnique.Taa : choice == AaChoice.Fxaa ? AaTechnique.Fxaa : AaTechnique.Original;
            Settings = new AaSettings(technique, Settings.Model, Settings.Resolution);
            UpdateNative();
        }
        public void SelectResolution(ResolutionMode resolution)
        {
            if (!ResolutionEnabled) throw new InvalidOperationException("This technique has no resolution mode.");
            Settings = new AaSettings(Settings.Technique, Settings.Model, resolution);
        }
        public void SelectConfiguration(int index)
        {
            if (Choice == AaChoice.Dlss)
            {
                if (index < 0 || index > (int)ModelSelection.TransformerM) throw new ArgumentOutOfRangeException(nameof(index));
                Settings = new AaSettings(Settings.Technique, (ModelSelection)index, Settings.Resolution);
            }
            else if (Choice == AaChoice.Msaa)
            {
                if (index < 0 || index > 2) throw new ArgumentOutOfRangeException(nameof(index));
                MsaaSamples = 2 << index;
            }
            else throw new InvalidOperationException("This technique has no configurable options.");
            UpdateNative();
        }
        private void UpdateNative()
        {
            NativeMsaa = Choice == AaChoice.Msaa ? MsaaSamples : 0;
            // Post-AA keeps the native fallback available; explicit Off and MSAA
            // do not leave an invisible FXAA toggle active behind the new controls.
            NativeFxaa = Choice == AaChoice.Fxaa || Choice == AaChoice.Taa || Choice == AaChoice.Dlss;
        }
    }
}
