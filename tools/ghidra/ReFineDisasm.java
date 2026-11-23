// Desensambla un rango (sin bytes) para diagnostico; util donde el decompilador no recupera
// una tabla de saltos.
//
//   analyzeHeadless work/ghidra ReFine2 -process EBOOT.elf -noanalysis -readOnly \
//       -scriptPath tools/ghidra -postScript ReFineDisasm.java 0x50e414 0x50e600
//
// Pares inicio fin. Si el rango no esta desensamblado, lo desensambla en memoria (solo lectura:
// el proyecto no se guarda).
//@category Re:Fine

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class ReFineDisasm extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] a = getScriptArgs();
        for (int i = 0; i + 1 < a.length; i += 2) {
            Address s = toAddr(Long.decode(a[i]));
            Address e = toAddr(Long.decode(a[i + 1]));
            println("==== " + s + " .. " + e);
            for (Address p = s; p.compareTo(e) < 0; p = p.add(4)) {
                Instruction ins = getInstructionAt(p);
                if (ins == null) { disassemble(p); ins = getInstructionAt(p); }
                println(p + "  " + (ins == null ? "??" : ins.toString()));
            }
        }
    }
}
