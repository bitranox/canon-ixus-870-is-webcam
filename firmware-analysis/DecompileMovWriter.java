// Decompile task_MovWrite and its case handlers (file create, write, close)
// Also decompile FUN_ff852314 (file open) and FUN_ff823fd4 (mkdir?)
// @category Analysis

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

public class DecompileMovWriter extends GhidraScript {

    private DecompInterface decomp;
    private PrintWriter pw;

    private void decompileAt(long addr, String label) {
        Address a = toAddr(addr);
        Function f = currentProgram.getFunctionManager().getFunctionContaining(a);
        if (f == null) {
            // Try creating function at address
            try {
                currentProgram.getFunctionManager().createFunction(
                    label, a, null, ghidra.program.model.symbol.SourceType.USER_DEFINED);
                f = currentProgram.getFunctionManager().getFunctionAt(a);
            } catch (Exception e) {
                pw.println("=== " + label + " @ " + String.format("0x%08x", addr) + " ===");
                pw.println("ERROR: Could not create function: " + e.getMessage());
                pw.println();
                return;
            }
        }
        if (f == null) {
            pw.println("=== " + label + " @ " + String.format("0x%08x", addr) + " ===");
            pw.println("ERROR: No function found");
            pw.println();
            return;
        }
        DecompileResults result = decomp.decompileFunction(f, 60, monitor);
        pw.println("=== " + label + " @ " + f.getEntryPoint() + " ===");
        if (result != null && result.decompileCompleted()) {
            pw.println(result.getDecompiledFunction().getC());
        } else {
            pw.println("ERROR: Decompilation failed");
        }
        pw.println();
    }

    @Override
    public void run() throws Exception {
        String outPath = getSourceFile().getParentFile().getAbsolutePath() +
            File.separator + "movwriter_decompiled.txt";
        pw = new PrintWriter(new FileWriter(outPath));
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        pw.println("=== task_MovWrite and File I/O Chain ===");
        pw.println();

        // task_MovWrite — the main message loop
        // From Ghidra: processes case 1 (create), case 2 (write), case 7 (close)
        decompileAt(0xFF92F1ECL, "task_MovWrite");

        // FUN_ff9309e4 — case 1 handler (file creation + init)
        decompileAt(0xFF9309E4L, "FUN_ff9309e4_case1_create");

        // FUN_ff852314 — file open (called from case 1)
        decompileAt(0xFF852314L, "FUN_ff852314_file_open");

        // FUN_ff823fd4 — mkdir? (called from case 1 before file open)
        decompileAt(0xFF823FD4L, "FUN_ff823fd4_mkdir");

        // FUN_ff85235c — file write (called from case 2 write handler)
        decompileAt(0xFF85235CL, "FUN_ff85235c_file_write");

        // FUN_ff8555b0 — called at start of case 1 init
        decompileAt(0xFF8555B0L, "FUN_ff8555b0");

        // FUN_ff853c08 — called after ff8555b0
        decompileAt(0xFF853C08L, "FUN_ff853c08");

        // FUN_ff851f18 — called before mkdir
        decompileAt(0xFF851F18L, "FUN_ff851f18");

        pw.close();
        decomp.dispose();
        println("Output: " + outPath);
    }
}
