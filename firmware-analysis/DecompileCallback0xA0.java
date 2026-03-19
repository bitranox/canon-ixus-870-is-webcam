// Decompile the callback at FF85DDA8 (+0xA0 callback set by msg 2)
// This is a code label inside a function, so we try to find and decompile the containing function
// @category Analysis
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.Address;
import java.io.*;

public class DecompileCallback0xA0 extends GhidraScript {
    @Override
    public void run() throws Exception {
        StringBuilder out = new StringBuilder();

        // Read raw ARM instructions at 0xFF85DDA8
        Address addr = toAddr(0xFF85DDA8L);
        out.append("Raw ARM instructions at 0xFF85DDA8:\n");
        for (int i = 0; i < 20; i++) {
            Address a = addr.add(i * 4);
            byte[] bytes = new byte[4];
            currentProgram.getMemory().getBytes(a, bytes);
            long val = (bytes[0] & 0xFFL) | ((bytes[1] & 0xFFL) << 8) |
                       ((bytes[2] & 0xFFL) << 16) | ((bytes[3] & 0xFFL) << 24);

            Instruction inst = currentProgram.getListing().getInstructionAt(a);
            String disasm = inst != null ? inst.toString() : "no instruction";
            out.append(String.format("  0x%08X: %08X  %s\n", a.getOffset(), val, disasm));
        }

        // Also get the containing function
        FunctionManager fm = currentProgram.getFunctionManager();
        Function func = fm.getFunctionContaining(addr);
        if (func != null) {
            out.append(String.format("\nContaining function: %s @ 0x%s (size=%d)\n",
                func.getName(), func.getEntryPoint(), func.getBody().getNumAddresses()));
        } else {
            out.append("\nNo containing function found\n");
        }

        // Also read FF85DD14 (the no-op callback)
        Address noop = toAddr(0xFF85DD14L);
        out.append("\nRaw ARM instructions at 0xFF85DD14 (no-op callback):\n");
        for (int i = 0; i < 5; i++) {
            Address a = noop.add(i * 4);
            byte[] bytes = new byte[4];
            currentProgram.getMemory().getBytes(a, bytes);
            long val = (bytes[0] & 0xFFL) | ((bytes[1] & 0xFFL) << 8) |
                       ((bytes[2] & 0xFFL) << 16) | ((bytes[3] & 0xFFL) << 24);

            Instruction inst = currentProgram.getListing().getInstructionAt(a);
            String disasm = inst != null ? inst.toString() : "no instruction";
            out.append(String.format("  0x%08X: %08X  %s\n", a.getOffset(), val, disasm));
        }

        // Read the entire sub_FF85DE1C (msg 2 handler) to see where +0xA0 callback is set
        Address msg2 = toAddr(0xFF85DE1CL);
        Function msg2Func = fm.getFunctionAt(msg2);
        if (msg2Func != null) {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            DecompileResults results = decomp.decompileFunction(msg2Func, 30, monitor);
            if (results != null && results.decompileCompleted()) {
                out.append("\n\n=== sub_FF85DE1C (msg 2 handler) ===\n");
                out.append(results.getDecompiledFunction().getC());
            }
            decomp.dispose();
        }

        String outPath = getSourceFile().getParentFile().getAbsolutePath()
            + "/callback_0xa0_analysis.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(outPath));
        pw.print(out.toString());
        pw.close();
        println("Wrote " + out.length() + " chars to " + outPath);
    }
}
