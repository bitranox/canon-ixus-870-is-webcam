// DecompileSIOISR.java — Ghidra headless script
// Decompile the SIO interrupt handler at 0xFF842754 and its callees.
// This is the ISR that reads PCM audio samples from the SIO data register.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileSIOISR extends GhidraScript {

    private static final long[] ADDRESSES = {
        0xff842754L,  // LAB_ff842754 — SIO ISR (registered via FUN_ff81ae8c)
        0xff842824L,  // FUN_ff842824 — SIO driver init (registers ISR for 8 channels)

        // SIO data table at 0xFFB53C88 — read register addresses and nearby entries
        // Functions that reference this table area
        0xff843500L,  // Near SIO driver — might handle received data
        0xff843600L,  // Near SIO driver
        0xff843700L,  // Near SIO driver
        0xff843800L,  // Near SIO driver
        0xff843900L,  // Near SIO driver — references 0xC022F000

        // Audio recording integration: who sends audio to task_MovWrite?
        // From the SIO data path, samples go somewhere. Let's find where.
        0xff845000L,  // SIO/Audio area
        0xff845200L,  // Near 0xC0220000 reference
        0xff845400L,  // Near 0xC0224000 reference
    };

    private static final String[] NAMES = {
        "LAB_ff842754 — SIO ISR (audio sample receive)",
        "FUN_ff842824 — SIO driver init",
        "FUN_ff843500 — SIO data handler?",
        "FUN_ff843600 — SIO area",
        "FUN_ff843700 — SIO area",
        "FUN_ff843800 — SIO area",
        "FUN_ff843900 — SIO area (0xC022F000 ref)",
        "FUN_ff845000 — SIO/Audio area",
        "FUN_ff845200 — near AudioIC refs",
        "FUN_ff845400 — near 0xC0224000 ref",
    };

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\sio_isr_decompiled.txt";

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("SIO Interrupt Handler — PCM Sample Path\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("========================================================================\n\n");

        // Read the SIO channel data table
        output.append("=== SIO Channel Data Table (0xFFB53C80-0xFFB53D00) ===\n\n");
        for (long a = 0xFFB53C80L; a < 0xFFB53D00L; a += 4) {
            try {
                int val = mem.getInt(toAddr(a));
                if (val != 0) {
                    output.append(String.format("  0x%08X = 0x%08X", a, val & 0xFFFFFFFFL));
                    if ((val & 0xFF000000) == 0xC0000000) output.append("  (I/O reg)");
                    if (val > 0x1000 && val < 0x04000000) output.append("  (RAM)");
                    output.append("\n");
                }
            } catch (Exception e) {}
        }
        output.append("\n");

        // Read SIO driver state (DAT_ff842b64, DAT_ff842b68, DAT_ff842b6c, DAT_ff842b84, DAT_ff842b88)
        output.append("=== SIO Driver ROM Constants ===\n\n");
        long[] sioConsts = {
            0xff842b64L, 0xff842b68L, 0xff842b6cL, 0xff842b70L,
            0xff842b74L, 0xff842b78L, 0xff842b84L, 0xff842b88L, 0xff842b8cL
        };
        for (long a : sioConsts) {
            try {
                int val = mem.getInt(toAddr(a));
                output.append(String.format("  DAT_%08X = 0x%08X\n", a, val & 0xFFFFFFFFL));
            } catch (Exception e) {}
        }
        output.append("\n");

        // Decompile functions
        for (int i = 0; i < ADDRESSES.length; i++) {
            Address addr = toAddr(ADDRESSES[i]);
            Function func = fm.getFunctionAt(addr);

            output.append("========================================================================\n");
            output.append(String.format("Function %d/%d: %s\n", i + 1, ADDRESSES.length, NAMES[i]));
            output.append(String.format("Address: 0x%08X\n", ADDRESSES[i]));
            output.append("========================================================================\n");

            if (func == null) {
                func = createFunction(addr, null);
            }

            if (func != null) {
                output.append("Ghidra name: " + func.getName() + "\n");
                output.append("Size: " + func.getBody().getNumAddresses() + " bytes\n\n");

                DecompileResults results = decomp.decompileFunction(func, 30, monitor);
                if (results.decompileCompleted()) {
                    output.append(results.getDecompiledFunction().getC() + "\n\n");
                } else {
                    output.append("[DECOMPILATION FAILED]\n\n");
                }
            } else {
                output.append("[NO FUNCTION AT THIS ADDRESS]\n\n");
            }
        }

        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();
        println("SIO ISR decompilation written to: " + OUTPUT_FILE);
    }
}
