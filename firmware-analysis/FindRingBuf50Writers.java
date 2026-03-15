// Find all functions that write to ring buffer +0x50
// @category Analysis

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;
import java.util.ArrayList;
import java.util.List;

public class FindRingBuf50Writers extends GhidraScript {

    @Override
    public void run() throws Exception {
        String outPath = getSourceFile().getParentFile().getAbsolutePath() +
            File.separator + "ringbuf_50_writers.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(outPath));

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        pw.println("=== Ring Buffer +0x50 Writer Analysis ===");
        pw.println();

        long[] dataAddrVals = { 0xFF93050CL, 0xFF92EC0CL };

        Set<String> processedFuncs = new HashSet<String>();
        int totalWriters = 0;

        for (int di = 0; di < dataAddrVals.length; di++) {
            Address dataAddr = toAddr(dataAddrVals[di]);
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(dataAddr);
            while (refs.hasNext()) {
                Reference ref = refs.next();
                Address fromAddr = ref.getFromAddress();
                Function func = currentProgram.getFunctionManager().getFunctionContaining(fromAddr);
                if (func == null) continue;

                String funcKey = func.getEntryPoint().toString();
                if (processedFuncs.contains(funcKey)) continue;
                processedFuncs.add(funcKey);

                DecompileResults result = decomp.decompileFunction(func, 30, monitor);
                if (result == null || !result.decompileCompleted()) continue;

                String code = result.getDecompiledFunction().getC();
                String[] lines = code.split("\n");
                List<String> writeLines = new ArrayList<String>();

                for (int i = 0; i < lines.length; i++) {
                    String trimmed = lines[i].trim();
                    if (trimmed.contains("+ 0x50)") && trimmed.contains("=") &&
                        !trimmed.contains("==") && !trimmed.contains("!=") &&
                        trimmed.contains("*(")) {
                        writeLines.add(trimmed);
                    }
                }

                if (!writeLines.isEmpty()) {
                    totalWriters++;
                    pw.println("========================================================================");
                    pw.println("Function: " + func.getName() + " @ " + func.getEntryPoint());
                    pw.println("Data ref: " + dataAddr);
                    pw.println();
                    pw.println("Lines writing to +0x50:");
                    for (int i = 0; i < writeLines.size(); i++) {
                        pw.println("  >> " + writeLines.get(i));
                    }
                    pw.println();
                    pw.println("Full decompilation:");
                    pw.println(code);
                    pw.println();
                }
            }
        }

        pw.println("=== Done: " + processedFuncs.size() + " functions, " + totalWriters + " writers ===");
        pw.close();
        decomp.dispose();
        println("Output: " + outPath);
        println("Found " + totalWriters + " functions writing to +0x50");
    }
}
