// Export the current program (with all user renames/comments/analysis) to a packed .gzf file. Arg: <out.gzf>
// Uses DomainFile.packFile (same format as File > Export > Ghidra Zip File); import back via File > Import / -import.
//@category DS2
import ghidra.app.script.GhidraScript;
import java.io.File;

public class ExportGzf extends GhidraScript {
    @Override
    protected void run() throws Exception {
        File out = new File(getScriptArgs()[0]);
        if (out.exists()) out.delete();
        currentProgram.getDomainFile().packFile(out, monitor);
        println("ExportGzf: " + out + " " + out.length() + " bytes");
    }
}
