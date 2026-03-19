// Decompile JPCORE completion callbacks and encode state struct helpers
// @category Analysis
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import java.io.*;
import java.util.*;

public class DecompileCallbacks extends GhidraScript {
    private DecompInterface decomp;
    private Set<Long> visited = new HashSet<>();
    private StringBuilder output = new StringBuilder();

    @Override
    public void run() throws Exception {
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        decomp.setSimplificationStyle("decompile");

        long[] targets = {
            0xFF8F18A4L,  // Non-first-frame JPCORE completion callback
            0xFF8ED6DCL,  // First-frame callback (registered by FUN_ff8eda90)
            0xFF8ED76CL,  // Callback address parameter to FUN_ff8f1654
            0xFF8F1E58L,  // DAT value — first-frame callback stored at DAT_ff8f12ac+0x18
            0xFF8F0720L,  // Function pointer at DAT_ffae3004
            0xFF8ED62CL,  // Function pointer at DAT_ff8edd24
        };

        // Also read the DAT_ff8f1e58 value
        try {
            Address datAddr = toAddr(0xFF8F1E58L);
            byte[] bytes = new byte[4];
            currentProgram.getMemory().getBytes(datAddr, bytes);
            long val = (bytes[0] & 0xFFL) | ((bytes[1] & 0xFFL) << 8) |
                       ((bytes[2] & 0xFFL) << 16) | ((bytes[3] & 0xFFL) << 24);
            output.append(String.format("DAT_ff8f1e58 = 0x%08X\n\n", val));
        } catch (Exception e) {
            output.append("Could not read DAT_ff8f1e58: " + e.getMessage() + "\n\n");
        }

        // Read DAT_ff8f12ac
        try {
            Address datAddr = toAddr(0xFF8F12ACL);
            byte[] bytes = new byte[4];
            currentProgram.getMemory().getBytes(datAddr, bytes);
            long val = (bytes[0] & 0xFFL) | ((bytes[1] & 0xFFL) << 8) |
                       ((bytes[2] & 0xFFL) << 16) | ((bytes[3] & 0xFFL) << 24);
            output.append(String.format("DAT_ff8f12ac = 0x%08X (JPCORE encode state base)\n\n", val));
        } catch (Exception e) {
            output.append("Could not read DAT_ff8f12ac: " + e.getMessage() + "\n\n");
        }

        for (long addr : targets) {
            decompileAt(addr, 0, 2);
        }

        String outPath = getSourceFile().getParentFile().getAbsolutePath()
            + File.separator + "callbacks_decompiled.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(outPath));
        pw.print(output.toString());
        pw.close();
        println("Wrote " + output.length() + " chars to " + outPath);
    }

    private void decompileAt(long addr, int depth, int maxDepth) {
        if (visited.contains(addr)) return;
        if (depth > maxDepth) return;
        visited.add(addr);

        Address address = toAddr(addr);
        FunctionManager fm = currentProgram.getFunctionManager();
        Function func = fm.getFunctionAt(address);
        if (func == null) {
            output.append(String.format("\n// No function at 0x%08X\n", addr));
            return;
        }

        output.append("\n========================================\n");
        output.append(String.format("Function: %s @ 0x%08X (size=%d, depth=%d)\n",
                      func.getName(), addr, func.getBody().getNumAddresses(), depth));
        output.append("========================================\n");

        Set<Function> callers = func.getCallingFunctions(monitor);
        if (!callers.isEmpty()) {
            output.append("// Called from:\n");
            for (Function c : callers) {
                output.append(String.format("//   0x%s (%s)\n", c.getEntryPoint(), c.getName()));
            }
        }
        Set<Function> callees = func.getCalledFunctions(monitor);
        if (!callees.isEmpty()) {
            output.append("// Calls:\n");
            for (Function c : callees) {
                output.append(String.format("//   0x%s (%s)\n", c.getEntryPoint(), c.getName()));
            }
        }

        DecompileResults results = decomp.decompileFunction(func, 30, monitor);
        if (results != null && results.decompileCompleted()) {
            output.append("\n" + results.getDecompiledFunction().getC() + "\n");
        } else {
            output.append("// DECOMPILATION FAILED\n");
        }

        if (depth < maxDepth) {
            for (Function callee : callees) {
                long a = callee.getEntryPoint().getOffset();
                if (a >= 0xFF800000L && a < 0xFFF00000L) {
                    decompileAt(a, depth + 1, maxDepth);
                }
            }
        }
    }
}
