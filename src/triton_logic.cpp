
#include "triton_logic.hpp"
#include "globals.hpp"
#include "utils.hpp"
#include "tainting_n_symbolic.hpp"
#include "context.hpp"
#include "blacklist.hpp"

#include <ida.hpp>
#include <dbg.hpp>

/*This function will create and fill the Triton object for every instruction
    Returns:
    0 instruction tritonized
    1 trigger is not activated
    2 other error*/
int tritonize(ea_t pc, thid_t threadID)
{
    /*Check that the runtime Trigger is on just in case*/
    if (!ponce_runtime_status.runtimeTrigger.getState())
        return 1;

    threadID = threadID ? threadID : get_current_thread();

    if (pc == 0) {
        msg("[!] Some error at tritonize since pc is 0\n");
        return 2;
    }

    //We delete the last_instruction
    if (ponce_runtime_status.last_triton_instruction != nullptr) {
        delete ponce_runtime_status.last_triton_instruction;
        ponce_runtime_status.last_triton_instruction = nullptr;
    }

    triton::arch::Instruction* tritonInst = new triton::arch::Instruction();
    ponce_runtime_status.last_triton_instruction = tritonInst;

    /*This will fill the 'cmd' (to get the instruction size) which is a insn_t structure https://www.hex-rays.com/products/ida/support/sdkdoc/classinsn__t.html */
    if (!can_decode(pc)) {
        msg("[!] Some error decoding instruction at " MEM_FORMAT "\n", pc);
    }

    unsigned char opcodes[15];
    ssize_t item_size = 0x0;

    insn_t ins;
    decode_insn(&ins, pc);
    item_size = ins.size;
    assert(item_size < sizeof(opcodes));
    get_bytes(&opcodes, item_size, pc, GMB_READALL, NULL);

    /* Setup Triton information */
    tritonInst->clear(); // ToDo: I think this is not necesary
    tritonInst->setOpcode((triton::uint8*)opcodes, item_size);
    tritonInst->setAddress(pc);
    tritonInst->setThreadId(threadID);

    try {
        if (!api.processing(*tritonInst)) {
            msg("[!] Instruction at " MEM_FORMAT " not supported by Triton: %s (Thread id: %d)\n", pc, tritonInst->getDisassembly().c_str(), threadID);
            return 2;
        }
    }
    catch (const triton::exceptions::Exception& e) {
        msg("[!] Instruction at " MEM_FORMAT " not supported by Triton: %s (Thread id: %d)\n", pc, tritonInst->getDisassembly().c_str(), threadID);
        return 2;
    }

    if (cmdOptions.showExtraDebugInfo) {
        msg("[+] Triton at " MEM_FORMAT " : %s (Thread id: %d)\n", pc, tritonInst->getDisassembly().c_str(), threadID);
    }

    /*In the case that the snapshot engine is in use we should track every memory write access*/
    if (snapshot.exists())
    {
        auto store_access_list = tritonInst->getStoreAccess();
        for (auto it = store_access_list.begin(); it != store_access_list.end(); it++)
        {
            triton::arch::MemoryAccess memory_access = it->first;
            auto addr = memory_access.getAddress();
            //This is the way to force IDA to read the value from the debugger
            //More info here: https://www.hex-rays.com/products/ida/support/sdkdoc/dbg_8hpp.html#ac67a564945a2c1721691aa2f657a908c
            invalidate_dbgmem_contents((ea_t)addr, memory_access.getSize()); //ToDo: Do I have to call this for every byte in memory I want to read?
            for (unsigned int i = 0; i < memory_access.getSize(); i++)
            {
                triton::uint128 value = 0;
                //We get the memory readed
                get_bytes(&value, 1, (ea_t)addr + i, GMB_READALL, NULL);

                //We add a meomory modification to the snapshot engine
                snapshot.addModification((ea_t)addr + i, value.convert_to<char>());
            }
        }
    }

    if (cmdOptions.addCommentsControlledOperands)
        get_controlled_operands_and_add_comment(tritonInst, pc);

    if (cmdOptions.addCommentsSymbolicExpresions)
        add_symbolic_expressions(tritonInst, pc);

    if (cmdOptions.paintExecutedInstructions)
    {
        //We only paint the executed instructions if they don't have a previous color
        if (get_item_color(pc) == 0xffffffff) {
            set_item_color(pc, cmdOptions.color_executed_instruction);
            ponce_comments.push_back(std::make_pair(pc, 3));
        }
    }

    //ToDo: The isSymbolized is missidentifying like "user-controlled" some instructions: https://github.com/JonathanSalwan/Triton/issues/383
    if (tritonInst->isTainted() || tritonInst->isSymbolized())
    {
        ponce_runtime_status.total_number_symbolic_ins++;

        if (cmdOptions.showDebugInfo) {
            msg("[!] Instruction %s at " MEM_FORMAT " \n", tritonInst->isTainted() ? "tainted" : "symbolized", pc);
        }
        if (cmdOptions.RenameTaintedFunctionNames)
            rename_tainted_function(pc);
        // Check if it is a conditional jump
        // We only color with a different color the symbolic conditions, to show the user he could do additional actions like solve
        if (tritonInst->isBranch())
        {
            ponce_runtime_status.total_number_symbolic_conditions++;
            if (cmdOptions.use_symbolic_engine)
                set_item_color(pc, cmdOptions.color_tainted_condition);
            else
                set_item_color(pc, cmdOptions.color_tainted);
            ponce_comments.push_back(std::make_pair(pc, 3));
        }
    }

    if (tritonInst->isBranch() && tritonInst->isSymbolized())
    {


        ea_t addr1 = (ea_t)tritonInst->getNextAddress();
        ea_t addr2 = (ea_t)tritonInst->operands[0].getImmediate().getValue();
        if (cmdOptions.showDebugInfo) {
            msg("[+] Branch symbolized detected at " MEM_FORMAT ": " MEM_FORMAT " or " MEM_FORMAT ", Taken:%s\n", pc, addr1, addr2, tritonInst->isConditionTaken() ? "Yes" : "No");
        }
       /* triton::usize ripId = 0;
        ripId = api.getSymbolicRegister(REG_XIP)->getId();

        if (tritonInst->isConditionTaken())
            ponce_runtime_status.myPathConstraints.push_back(new PathConstraint(ripId, pc, addr2, addr1, ponce_runtime_status.myPathConstraints.size()));
        else
            ponce_runtime_status.myPathConstraints.push_back(new PathConstraint(ripId, pc, addr1, addr2, ponce_runtime_status.myPathConstraints.size()));*/
    }
    return 0;
    //We add the instruction to the map, so we can use it later to negate conditions, view SE, slicing, etc..
    //instructions_executed_map[pc].push_back(tritonInst);
}

bool ponce_set_triton_architecture() {
    if (ph.id == PLFM_386) {
        if (ph.use64())
            api.setArchitecture(triton::arch::ARCH_X86_64);
        else if (ph.use32())
            api.setArchitecture(triton::arch::ARCH_X86);
        else {
            msg("[!] Wrong architecture\n");
            return false;
        }
    }
    else if (ph.id == PLFM_ARM) {
        if (ph.use64())
            api.setArchitecture(triton::arch::ARCH_AARCH64);
        else if (ph.use32())
            api.setArchitecture(triton::arch::ARCH_ARM32);
        else {
            msg("[!] Wrong architecture\n");
            return false;
        }
    }
    else {
        msg("[!] Architecture not supported by Ponce\n");
        return false;
    }
    return true;
}

/*This functions is called every time a new debugger session starts*/
void triton_restart_engines()
{
    if (cmdOptions.showDebugInfo)
        msg("[+] Restarting triton engines...\n");
    //We need to set the architecture for Triton
    ponce_set_triton_architecture();
    //We reset everything at the beginning
    api.reset();
    // Memory access callback
    api.addCallback(needConcreteMemoryValue_cb);
    // Register access callback
    api.addCallback(needConcreteRegisterValue_cb);
    //If we are in taint analysis mode we enable only the tainting engine and disable the symbolic one
    api.getTaintEngine()->enable(cmdOptions.use_tainting_engine);
    api.getSymbolicEngine()->enable(cmdOptions.use_symbolic_engine);
    if (ponce_runtime_status.last_triton_instruction) {
        delete ponce_runtime_status.last_triton_instruction;
        ponce_runtime_status.last_triton_instruction = nullptr;
    }

    // This optimization is veeery good for the size of the formulas
    //api.enableSymbolicOptimization(triton::engines::symbolic:: ALIGNED_MEMORY, true);
    //api.setMode(triton::modes::ALIGNED_MEMORY, true);

    // We only are symbolic or taint executing an instruction if it is tainted, so it is a bit faster and we save a lot of memory
    //if (cmdOptions.only_on_optimization)
    //{
    //	if (cmdOptions.use_symbolic_engine)
    //	{
    //		api.setMode(triton::modes::ONLY_ON_SYMBOLIZED, true);
    //		/*api.enableSymbolicOptimization(triton::engines::symbolic::AST_DICTIONARIES, true); // seems not to exist any more
    //		api.enableSymbolicOptimization(triton::engines::symbolic::ONLY_ON_SYMBOLIZED, true);*/
    //	}
    //	if (cmdOptions.use_tainting_engine)
    //	{
    //		//We need to disable this optimization using the taint engine, if not a lot of RAM is consumed
    //		api.setMode(triton::modes::ONLY_ON_SYMBOLIZED, true); 
    //		/*api.enableSymbolicOptimization(triton::engines::symbolic::AST_DICTIONARIES, false);
    //		api.enableSymbolicOptimization(triton::engines::symbolic::ONLY_ON_TAINTED, true);*/
    //	}
    //}
    ponce_runtime_status.runtimeTrigger.disable();
    ponce_runtime_status.is_ponce_tracing_enabled = false;
    ponce_runtime_status.tainted_functions_index = 0;
    //Reset instruction counter
    ponce_runtime_status.total_number_traced_ins = 0;
    ponce_runtime_status.total_number_symbolic_ins = 0;
    ponce_runtime_status.total_number_symbolic_conditions = 0;
    ponce_runtime_status.current_trace_counter = 0;
    breakpoint_pending_actions.clear();
    set_automatic_taint_n_simbolic();
    ponce_runtime_status.myPathConstraints.clear();
}


/*This function is call the first time we are tainting something to enable the trigger, the flags and the tracing*/
void start_tainting_or_symbolic_analysis()
{
    if (!ponce_runtime_status.is_ponce_tracing_enabled)
    {
        triton_restart_engines();
        // Delete previous Ponce comments
        delete_ponce_comments();
        ponce_runtime_status.runtimeTrigger.enable();
        ponce_runtime_status.analyzed_thread = get_current_thread();
        ponce_runtime_status.is_ponce_tracing_enabled = true;
        enable_step_trace(true);
        set_step_trace_options(0);
        ponce_runtime_status.tracing_start_time = 0;
    }

}