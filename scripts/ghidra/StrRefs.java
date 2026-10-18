// For each string argument: find memory occurrences (ASCII, NUL-terminated, exact), list references and containing functions.
// Args: <outfile> <string>...   (prefix a string with "~" for substring match inside a longer string)
//@category DS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;

public class StrRefs extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Memory mem = currentProgram.getMemory();
        try (PrintWriter pw = new PrintWriter(new FileWriter(args[0], true))) {
            for (int i = 1; i < args.length; i++) {
                String s = args[i];
                boolean sub = s.startsWith("~");
                if (sub) s = s.substring(1);
                byte[] pat = s.getBytes(StandardCharsets.US_ASCII);
                pw.println("== \"" + args[i] + "\"");
                Address a = mem.getMinAddress();
                int hits = 0;
                while (a != null && hits < 50) {
                    a = mem.findBytes(a, pat, null, true, monitor);
                    if (a == null) break;
                    boolean ok = true;
                    if (!sub) {
                        try {
                            byte before = mem.getByte(a.subtract(1));
                            byte after = mem.getByte(a.add(pat.length));
                            ok = before == 0 && after == 0;
                        } catch (Exception e) { ok = false; }
                    }
                    if (ok) {
                        hits++;
                        pw.println("  @" + a);
                        for (Reference r : getReferencesTo(a)) {
                            Function f = getFunctionContaining(r.getFromAddress());
                            pw.println("    ref " + r.getFromAddress() + " " + r.getReferenceType() + " in " +
                                (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
                        }
                    }
                    a = a.add(1);
                }
            }
        }
    }
}
