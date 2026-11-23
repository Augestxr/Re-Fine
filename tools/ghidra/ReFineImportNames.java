// Pone nombre a las funciones HLE (imports de firmware) del EBOOT usando la salida
// de ps3recomp/tools/ppu_loader.py, y deja el programa listo para analizar:
//
//   1. r2 (TOC) = module_toc de EBOOT.loader.json en todo el codigo. Sin esto el
//      decompilador no resuelve ningun acceso relativo al TOC.
//   2. Etiqueta el entry point (OPD y codigo).
//   3. Crea las funciones de EBOOT.functions.json que Ghidra no haya encontrado.
//   4. Cada trampolin de import (stub de 0x20 bytes) pasa a ser una funcion con el
//      nombre del firmware (cellGcmInit...) en el namespace de su modulo. Lee
//      EBOOT.named.imports.json (tools/name_imports.py) si existe; si no,
//      *.imports.json y nombra los stubs <modulo>_<NID>.
//
// Argumento: carpeta de salida del loader (work/loader_out). Sin argumento, la pide.
//   analyzeHeadless <proj> ReFine -process EBOOT.elf -noanalysis \
//       -scriptPath tools/ghidra -preScript ReFineImportNames.java work/loader_out
//
// Idioma al importar: PowerPC:BE:64:64-32addr (ver docs/decisions.md).
//@category Re:Fine

import java.io.File;
import java.io.FileReader;
import java.io.Reader;
import java.math.BigInteger;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class ReFineImportNames extends GhidraScript {

    private static final int STUB_SIZE = 0x20;

    private long moduleToc = -1;

    @Override
    protected void run() throws Exception {
        File dir;
        String[] args = getScriptArgs();
        if (args.length > 0) {
            dir = new File(args[0]);
        } else {
            dir = askDirectory("Salida de ppu_loader.py (work/loader_out)", "Usar");
        }
        if (!dir.isDirectory()) {
            throw new IllegalArgumentException("No es una carpeta: " + dir);
        }

        File loaderJson = findOne(dir, ".loader.json");
        if (loaderJson != null) {
            applyLoaderInfo(readJson(loaderJson).getAsJsonObject());
        } else {
            printerr("Sin *.loader.json: no se fija r2 ni el entry point");
        }

        File funcsJson = findOne(dir, ".functions.json");
        if (funcsJson != null) {
            createFunctions(readJson(funcsJson).getAsJsonArray());
        }

        File importsJson = findOne(dir, ".named.imports.json");
        if (importsJson == null) {
            importsJson = findOne(dir, ".imports.json");
        }
        if (importsJson == null) {
            throw new IllegalArgumentException("No hay *.imports.json en " + dir);
        }
        nameImports(readJson(importsJson).getAsJsonArray());
    }

    private void applyLoaderInfo(JsonObject info) throws Exception {
        long toc = hex(info.get("module_toc"));
        moduleToc = toc;
        Register r2 = currentProgram.getRegister("r2");
        AddressSet code = new AddressSet();
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            if (b.isExecute() && b.isInitialized()) {
                code.add(b.getStart(), b.getEnd());
            }
        }
        for (var range : code) {
            currentProgram.getProgramContext().setValue(r2, range.getMinAddress(),
                range.getMaxAddress(), BigInteger.valueOf(toc));
        }
        println(String.format("r2 (TOC) = 0x%x en %d rango(s) ejecutables", toc,
            code.getNumAddressRanges()));

        Address opd = addr(hex(info.get("entry_opd")));
        Address entry = addr(hex(info.get("entry_code")));
        createLabel(opd, "_start_opd", true, SourceType.USER_DEFINED);
        disassemble(entry);
        Function f = getFunctionAt(entry);
        if (f == null) {
            f = createFunction(entry, "_start");
        } else {
            f.setName("_start", SourceType.USER_DEFINED);
        }
        addEntryPoint(entry);
        println("entry: OPD " + opd + " -> codigo " + entry);
    }

    private void createFunctions(JsonArray funcs) throws Exception {
        int created = 0, otherToc = 0;
        Register r2 = currentProgram.getRegister("r2");
        monitor.initialize(funcs.size());
        monitor.setMessage("Funciones del loader");
        for (JsonElement e : funcs) {
            monitor.checkCancelled();
            monitor.incrementProgress(1);
            JsonObject fo = e.getAsJsonObject();
            Address start = addr(hex(fo.get("start")));
            // F enlaza librerias del SDK con su propia TOC (0xc0a280 frente a la del
            // modulo, 0xbfa3d8): r2 se fija por rango con la toc del descriptor OPD.
            if (fo.has("toc") && fo.has("end") && hex(fo.get("toc")) != moduleToc) {
                currentProgram.getProgramContext().setValue(r2, start,
                    addr(hex(fo.get("end")) - 1), BigInteger.valueOf(hex(fo.get("toc"))));
                otherToc++;
            }
            if (getFunctionAt(start) != null) {
                continue;
            }
            new DisassembleCommand(start, null, true).applyTo(currentProgram, monitor);
            if (createFunction(start, null) != null) {
                created++;
            }
        }
        println(String.format("funciones del loader: %d, creadas ahora: %d", funcs.size(), created));
        println(String.format("funciones con otra TOC (r2 por rango): %d", otherToc));
    }

    private void nameImports(JsonArray imports) throws Exception {
        SymbolTable st = currentProgram.getSymbolTable();
        int named = 0, unnamed = 0;
        for (JsonElement el : imports) {
            JsonObject e = el.getAsJsonObject();
            String lib = e.get("library").getAsString();
            long nid = hex(e.get("nid"));
            Address stub = addr(hex(e.get("stub")));
            String name;
            if (e.has("name") && !e.get("name").isJsonNull()) {
                name = e.get("name").getAsString();
                named++;
            } else {
                name = String.format("%s_%08X", lib, nid);
                unnamed++;
            }

            Namespace ns = st.getNamespace(lib, currentProgram.getGlobalNamespace());
            if (ns == null) {
                ns = st.createNameSpace(currentProgram.getGlobalNamespace(), lib,
                    SourceType.USER_DEFINED);
            }

            // El stub es una funcion propia de 0x20 bytes, igual que la trata el lifter.
            disassemble(stub);
            Function f = getFunctionAt(stub);
            if (f == null) {
                Function inside = getFunctionContaining(stub);
                if (inside != null && !inside.getEntryPoint().equals(stub)) {
                    // Ghidra lo junto con el stub anterior: se recorta ese cuerpo.
                    inside.setBody(new AddressSet(inside.getEntryPoint(), stub.subtract(1)));
                }
                f = createFunction(stub, name);
            }
            if (f == null) {
                printerr("No se pudo crear la funcion en " + stub + " (" + name + ")");
                continue;
            }
            f.setBody(new AddressSet(stub, stub.add(STUB_SIZE - 1)));
            // Idempotente: en una segunda pasada la funcion ya tiene nombre y namespace, y
            // puede quedar una etiqueta con el mismo nombre que haria fallar el cambio
            // (DuplicateNameException).
            Symbol fs = f.getSymbol();
            if (!fs.getName().equals(name) || !fs.getParentNamespace().equals(ns)) {
                for (Symbol s : st.getSymbols(stub)) {
                    if (!s.equals(fs) && s.getName().equals(name)) {
                        s.delete();
                    }
                }
                fs.setNameAndNamespace(name, ns, SourceType.USER_DEFINED);
            }
            setPlateComment(stub, String.format("HLE import: %s::%s  NID 0x%08X", lib, name, nid));
        }
        println(String.format("imports: %d (con nombre %d, sin nombre %d)",
            imports.size(), named, unnamed));
    }

    private File findOne(File dir, String suffix) {
        File[] hits = dir.listFiles((d, n) -> n.endsWith(suffix));
        if (hits == null || hits.length == 0) {
            return null;
        }
        if (hits.length > 1) {
            printerr("Varios *" + suffix + ", uso " + hits[0].getName());
        }
        return hits[0];
    }

    private JsonElement readJson(File f) throws Exception {
        try (Reader r = new FileReader(f)) {
            return JsonParser.parseReader(r);
        }
    }

    private long hex(JsonElement e) {
        String s = e.getAsString();
        return Long.parseLong(s.startsWith("0x") ? s.substring(2) : s, 16);
    }

    private Address addr(long off) {
        return currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(off);
    }
}
