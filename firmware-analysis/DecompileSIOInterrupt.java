// DecompileSIOInterrupt.java — Ghidra headless script
// Decompiles the SIO interrupt handler chain to find where audio PCM samples
// are accumulated into a RAM buffer.
//
// FUN_ff842cb0 is called by FUN_ff842d04 (SioDrv) — likely sets up the
// SIO interrupt or processes audio data.
// FUN_ff8465ac/bc are callbacks from FUN_ff847334 (ROM addresses from params).
// The ISR reads from 0xC0220080 and writes PCM samples to a buffer.

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileSIOInterrupt extends GhidraScript {

    private static final long[] ADDRESSES = {
        // SIO driver internals
        0xff842cb0L,  // Called by FUN_ff842d04 — SIO command handler
        0xff842b74L,  // DAT_ff842b74 — referenced by SioDrv

        // AudioTsk callbacks (ROM addresses from FUN_ff847334 literal pool)
        0xff8465acL,  // Callback 1 (DAT_ff847404 = 0xFF8465AC)
        0xff8465bcL,  // Callback 2 (DAT_ff847408 = 0xFF8465BC)

        // SIO driver struct base — DAT_ff842b8c points to register table
        // Need to find the ISR registration and the data accumulation function

        // Functions near the SIO driver that handle data reception
        0xff842a00L,  // SIO area — potential receive handler
        0xff842a80L,  // SIO area
        0xff842b00L,  // SIO area
        0xff842b40L,  // SIO area
        0xff842c00L,  // SIO area
        0xff842c40L,  // SIO area
        0xff842c80L,  // SIO area — right before ff842cb0

        // Audio data accumulation — functions called from movie recording
        // that might set up the audio buffer
        0xff930910L,  // Called by task_MovWrite case 4 (audio codec params)
        0xff92fb50L,  // Called by msg 7 handler (ring buffer cleanup)

        // SIO driver DAT values
        0xff842b8cL,  // SIO register table base (ROM literal)
    };

    private static final String[] NAMES = {
        "FUN_ff842cb0 — SIO command handler (called by SioDrv)",
        "FUN_ff842b74 — SioDrv referenced value",
        "FUN_ff8465ac — AudioTsk callback 1",
        "FUN_ff8465bc — AudioTsk callback 2",
        "FUN_ff842a00 — SIO area",
        "FUN_ff842a80 — SIO area",
        "FUN_ff842b00 — SIO area",
        "FUN_ff842b40 — SIO area",
        "FUN_ff842c00 — SIO area",
        "FUN_ff842c40 — SIO area",
        "FUN_ff842c80 — SIO area (before cb0)",
        "FUN_ff930910 — audio codec params (task_MovWrite)",
        "FUN_ff92fb50 — ring buffer cleanup (msg 7)",
        "DAT_ff842b8c — SIO register table base",
    };

    private static final String OUTPUT_FILE =
        "C:\\projects\\ixus870IS\\firmware-analysis\\sio_interrupt_decompiled.txt";

    @Override
    protected void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        Memory mem = currentProgram.getMemory();
        StringBuilder output = new StringBuilder();

        output.append("========================================================================\n");
        output.append("SIO Interrupt Handler Chain — Audio PCM Buffer Search\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("========================================================================\n\n");

        // Read key ROM data values
        output.append("--- Key ROM Data Values ---\n");
        long[] romAddrs = {
            0xff842b74L, 0xff842b78L, 0xff842b8cL, 0xff84328cL,
            0xff847404L, 0xff847408L,
        };
        String[] romNames = {
            "SioDrv param", "SioDrv string", "SIO register table", "SioDrv param2",
            "AudioTsk callback1 addr", "AudioTsk callback2 addr",
        };
        for (int i = 0; i < romAddrs.length; i++) {
            try {
                int val = mem.getInt(toAddr(romAddrs[i]));
                output.append(String.format("  0x%08X (%s) = 0x%08X", romAddrs[i], romNames[i], val & 0xFFFFFFFFL));
                // If it's a string address, try to read the string
                if (val > 0xFF800000L) {
                    try {
                        byte[] bytes = new byte[30];
                        mem.getBytes(toAddr(val & 0xFFFFFFFFL), bytes);
                        StringBuilder str = new StringBuilder();
                        for (byte b : bytes) {
                            if (b == 0) break;
                            if (b >= 32 && b < 127) str.append((char)b);
                        }
                        if (str.length() > 2) output.append(" = \"" + str + "\"");
                    } catch (Exception e) {}
                }
                output.append("\n");
            } catch (Exception e) {
                output.append(String.format("  0x%08X (%s) = [ERROR]\n", romAddrs[i], romNames[i]));
            }
        }

        // Read the SIO register table to understand the channel layout
        output.append("\n--- SIO Register Table ---\n");
        try {
            int tableBase = mem.getInt(toAddr(0xff842b8cL));
            output.append(String.format("  Table base (DAT_ff842b8c) = 0x%08X\n", tableBase & 0xFFFFFFFFL));
            // This is a ROM address or RAM address containing the channel register pointers
            // Try to read if it's in ROM
            if ((tableBase & 0xFF000000) == 0xFF000000) {
                for (int ch = 0; ch < 4; ch++) {
                    int offset = ch * 0x18;
                    output.append(String.format("  Channel %d (offset 0x%X):\n", ch, offset));
                    for (int w = 0; w < 6; w++) {
                        int val = mem.getInt(toAddr((tableBase & 0xFFFFFFFFL) + offset + w * 4));
                        output.append(String.format("    +0x%02X = 0x%08X\n", offset + w * 4, val & 0xFFFFFFFFL));
                    }
                }
            }
        } catch (Exception e) {
            output.append("  [ERROR reading table]\n");
        }
        output.append("\n");

        // Decompile functions
        for (int i = 0; i < ADDRESSES.length; i++) {
            Address addr = toAddr(ADDRESSES[i]);

            output.append("========================================================================\n");
            output.append(String.format("Function %d/%d: %s\n", i + 1, ADDRESSES.length, NAMES[i]));
            output.append(String.format("Address: 0x%08X\n", ADDRESSES[i]));
            output.append("========================================================================\n");

            // For DAT entries, just read the value
            if (NAMES[i].startsWith("DAT_")) {
                try {
                    int val = mem.getInt(addr);
                    output.append(String.format("Value: 0x%08X\n\n", val & 0xFFFFFFFFL));
                } catch (Exception e) {
                    output.append("[READ ERROR]\n\n");
                }
                continue;
            }

            Function func = fm.getFunctionAt(addr);
            if (func == null) {
                func = createFunction(addr, null);
            }

            if (func != null) {
                output.append("Ghidra name: " + func.getName() + "\n");
                output.append("Size: " + func.getBody().getNumAddresses() + " bytes\n");

                DecompileResults results = decomp.decompileFunction(func, 30, monitor);
                if (results.decompileCompleted()) {
                    output.append("\n" + results.getDecompiledFunction().getC() + "\n\n");
                } else {
                    output.append("\n[DECOMPILATION FAILED]\n\n");
                }
            } else {
                output.append("[NO FUNCTION AT THIS ADDRESS]\n\n");
            }
        }

        PrintWriter writer = new PrintWriter(new FileWriter(OUTPUT_FILE));
        writer.print(output.toString());
        writer.close();
        println("SIO interrupt decompilation written to: " + OUTPUT_FILE);
    }
}
