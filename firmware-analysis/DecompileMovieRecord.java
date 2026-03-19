// Ghidra headless script to decompile UIFS_StartMovieRecord_FW and its call targets
// Run with: analyzeHeadless <project_dir> <project_name> -process PRIMARY.BIN -noanalysis
//           -scriptPath <dir> -postScript DecompileMovieRecord.java
//
//@category CHDK
//@author webcam-re

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.DecompiledFunction;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.RefType;
import java.io.FileWriter;
import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class DecompileMovieRecord extends GhidraScript {

    private DecompInterface decomp;
    private StringBuilder output;
    private Set<Long> decompiled; // track already-decompiled addresses

    @Override
    public void run() throws Exception {
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        output = new StringBuilder();
        decompiled = new LinkedHashSet<>();

        output.append("========================================================================\n");
        output.append("UIFS_StartMovieRecord Decompilation (with call targets, 2 levels deep)\n");
        output.append("Firmware: IXUS 870 IS / SD 880 IS, version 1.01a\n");
        output.append("========================================================================\n\n");

        // --- Level 0: Top-level functions ---
        output.append("////////////////////////////////////////////////////////////////////////\n");
        output.append("// LEVEL 0: Top-level entry points\n");
        output.append("////////////////////////////////////////////////////////////////////////\n\n");

        // 1. j_UIFS_StartMovieRecord_FW (jump wrapper)
        decompileAt(0xFF932E64L, "j_UIFS_StartMovieRecord_FW");

        // 2. UIFS_StartMovieRecord_FW (main function)
        List<Long> level1Targets = decompileAndCollectCalls(0xFF883D50L, "UIFS_StartMovieRecord_FW");

        // --- Level 1: Functions called by UIFS_StartMovieRecord_FW ---
        output.append("\n////////////////////////////////////////////////////////////////////////\n");
        output.append("// LEVEL 1: Functions called by UIFS_StartMovieRecord_FW\n");
        output.append("////////////////////////////////////////////////////////////////////////\n\n");

        List<Long> level2Targets = new ArrayList<>();
        for (Long target : level1Targets) {
            if (!decompiled.contains(target)) {
                List<Long> subcalls = decompileAndCollectCalls(target, null);
                level2Targets.addAll(subcalls);
            }
        }

        // --- Level 2: Functions called by level 1 targets ---
        output.append("\n////////////////////////////////////////////////////////////////////////\n");
        output.append("// LEVEL 2: Functions called by level-1 targets\n");
        output.append("////////////////////////////////////////////////////////////////////////\n\n");

        for (Long target : level2Targets) {
            if (!decompiled.contains(target)) {
                decompileAt(target, null);
            }
        }

        // Write output to file
        String outputPath = "C:/projects/ixus870IS/firmware-analysis/movie_record_start_decompiled.txt";

        FileWriter writer = new FileWriter(outputPath);
        writer.write(output.toString());
        writer.close();

        decomp.dispose();

        println("\nDecompilation output written to: " + outputPath);
        println("Total functions decompiled: " + decompiled.size());
    }

    /**
     * Decompile the function at the given address and return the decompiled C code.
     * Returns null if decompilation fails.
     */
    private String decompileAt(long addr, String labelName) {
        Address address = toAddr(addr);

        if (decompiled.contains(addr)) {
            return null;
        }
        decompiled.add(addr);

        Function func = getFunctionAt(address);
        if (func == null) {
            try {
                func = createFunction(address, labelName != null ? labelName : ("FUN_" + Long.toHexString(addr)));
            } catch (Exception e) {
                output.append("// ERROR: Could not create function at " + address + ": " + e.getMessage() + "\n\n");
                return null;
            }
        }
        if (func == null) {
            output.append("// Could not find or create function at " + address + "\n\n");
            return null;
        }

        // Label the function if a name was provided
        if (labelName != null) {
            try {
                func.setName(labelName, SourceType.USER_DEFINED);
            } catch (Exception e) {
                // name conflict; ignore
            }
        }

        String funcName = func.getName();
        println("Decompiling " + funcName + " at " + address + "...");

        DecompileResults results = decomp.decompileFunction(func, 120, monitor);
        if (results == null || !results.decompileCompleted()) {
            output.append("// Decompilation failed for " + funcName + " at " + address + "\n\n");
            return null;
        }

        DecompiledFunction decompFunc = results.getDecompiledFunction();
        if (decompFunc == null) {
            output.append("// No decompiled output for " + funcName + " at " + address + "\n\n");
            return null;
        }

        String sig = decompFunc.getSignature();
        String code = decompFunc.getC();

        output.append("// === " + funcName + " @ " + address + " ===\n");
        output.append("// Signature: " + sig + "\n");
        output.append(code);
        output.append("\n\n");

        return code;
    }

    /**
     * Decompile the function at the given address AND collect call targets
     * using three methods:
     *   1) BL/BLX instruction scanning
     *   2) Ghidra reference manager (CALL references from the function body)
     *   3) Regex on the decompiled C output (FUN_xxxxxxxx names)
     * Returns combined list of unique call target addresses.
     */
    private List<Long> decompileAndCollectCalls(long addr, String labelName) {
        Address address = toAddr(addr);
        Set<Long> seen = new LinkedHashSet<>();
        List<Long> callTargets = new ArrayList<>();

        // Decompile and get C code
        String code = null;
        if (!decompiled.contains(addr)) {
            code = decompileAt(addr, labelName);
        }

        // Method 1: BL/BLX instruction scanning
        List<Long> instrTargets = collectCallTargetsFromInstructions(address);
        for (Long t : instrTargets) {
            if (!seen.contains(t)) {
                seen.add(t);
                callTargets.add(t);
            }
        }

        // Method 2: Ghidra reference manager
        List<Long> refTargets = collectCallTargetsFromReferences(address);
        for (Long t : refTargets) {
            if (!seen.contains(t)) {
                seen.add(t);
                callTargets.add(t);
            }
        }

        // Method 3: Parse FUN_ names from decompiled C and resolve to addresses
        if (code != null) {
            List<Long> codeTargets = collectCallTargetsFromDecompiledCode(code);
            for (Long t : codeTargets) {
                if (!seen.contains(t)) {
                    seen.add(t);
                    callTargets.add(t);
                }
            }
        }

        // Remove self-reference
        callTargets.remove(Long.valueOf(addr));

        if (!callTargets.isEmpty()) {
            String name = (labelName != null ? labelName : ("FUN_" + Long.toHexString(addr)));
            output.append("// Call targets from " + name + " (" + callTargets.size() + " unique):\n");
            for (Long target : callTargets) {
                Address tAddr = toAddr(target);
                Function tFunc = getFunctionAt(tAddr);
                String tName = (tFunc != null) ? tFunc.getName() : "FUN_" + Long.toHexString(target);
                output.append("//   -> " + tName + " @ " + tAddr + "\n");
            }
            output.append("\n");
        } else {
            output.append("// No call targets found for " +
                (labelName != null ? labelName : ("FUN_" + Long.toHexString(addr))) + "\n\n");
        }

        return callTargets;
    }

    /**
     * Scan the instruction body of the function for BL/BLX call instructions.
     */
    private List<Long> collectCallTargetsFromInstructions(Address funcAddr) {
        List<Long> targets = new ArrayList<>();
        Set<Long> seen = new LinkedHashSet<>();

        Function func = getFunctionAt(funcAddr);
        if (func == null) return targets;

        AddressSetView body = func.getBody();
        InstructionIterator instrIter = currentProgram.getListing().getInstructions(body, true);

        while (instrIter.hasNext()) {
            Instruction instr = instrIter.next();
            String mnemonic = instr.getMnemonicString();

            if (mnemonic.equals("bl") || mnemonic.equals("blx")) {
                for (int i = 0; i < instr.getNumOperands(); i++) {
                    Object[] opObjects = instr.getOpObjects(i);
                    for (Object obj : opObjects) {
                        if (obj instanceof Address) {
                            long targetAddr = ((Address) obj).getOffset();
                            if (targetAddr >= 0xFF800000L && targetAddr <= 0xFFFFFFFFL) {
                                if (!seen.contains(targetAddr)) {
                                    seen.add(targetAddr);
                                    targets.add(targetAddr);
                                }
                            }
                        }
                    }
                }
            }
        }

        return targets;
    }

    /**
     * Use Ghidra's reference manager to find CALL references from the function body.
     */
    private List<Long> collectCallTargetsFromReferences(Address funcAddr) {
        List<Long> targets = new ArrayList<>();
        Set<Long> seen = new LinkedHashSet<>();

        Function func = getFunctionAt(funcAddr);
        if (func == null) return targets;

        AddressSetView body = func.getBody();

        // Iterate through all addresses in the function body and check for call references
        ghidra.program.model.address.AddressIterator addrIter = body.getAddresses(true);
        while (addrIter.hasNext()) {
            Address addr = addrIter.next();
            Reference[] refs = currentProgram.getReferenceManager().getReferencesFrom(addr);
            for (Reference ref : refs) {
                if (ref.getReferenceType().isCall()) {
                    long targetAddr = ref.getToAddress().getOffset();
                    if (targetAddr >= 0xFF800000L && targetAddr <= 0xFFFFFFFFL) {
                        if (!seen.contains(targetAddr)) {
                            seen.add(targetAddr);
                            targets.add(targetAddr);
                        }
                    }
                }
            }
        }

        return targets;
    }

    /**
     * Parse FUN_xxxxxxxx and DAT_xxxxxxxx function calls from decompiled C code.
     * Resolves function names to addresses.
     */
    private List<Long> collectCallTargetsFromDecompiledCode(String code) {
        List<Long> targets = new ArrayList<>();
        Set<Long> seen = new LinkedHashSet<>();

        // Match FUN_xxxxxxxx patterns (function calls in decompiled output)
        Pattern pattern = Pattern.compile("FUN_([0-9a-fA-F]{8})");
        Matcher matcher = pattern.matcher(code);

        while (matcher.find()) {
            String hexAddr = matcher.group(1);
            try {
                long targetAddr = Long.parseUnsignedLong(hexAddr, 16);
                if (targetAddr >= 0xFF800000L && targetAddr <= 0xFFFFFFFFL) {
                    if (!seen.contains(targetAddr)) {
                        seen.add(targetAddr);
                        targets.add(targetAddr);
                    }
                }
            } catch (NumberFormatException e) {
                // skip
            }
        }

        return targets;
    }
}
