//
//  floyd_llvm_codegen.cpp
//  floyd_speak
//
//  Created by Marcus Zetterquist on 2019-03-23.
//  Copyright © 2019 Marcus Zetterquist. All rights reserved.
//

#include "floyd_llvm_codegen.h"
#include "ast_value.h"

#include <llvm/ADT/APInt.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include "llvm/Bitcode/BitstreamWriter.h"


#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <iostream>

#include "host_functions.h"
#include "compiler_basics.h"
#include "compiler_helpers.h"
#include "floyd_parser.h"
#include "pass3.h"
#include "text_parser.h"
#include "ast_json.h"

#include "quark.h"

//http://releases.llvm.org/2.6/docs/tutorial/JITTutorial2.html


/*
# ACCESSING INTEGER INSIDE GENERICVALUE

//const int x = value.IntVal.U.VAL;
//	const int64_t x = llvm::cast<llvm::ConstantInt>(value);
//	QUARK_TRACE_SS("Fib = " << x);

#if 0
if (llvm::ConstantInt* CI = llvm::dyn_cast<llvm::ConstantInt*>(value)) {
  if (CI->getBitWidth() <= 32) {
    const auto constIntValue = CI->getSExtValue();
    QUARK_TRACE_SS("Fib: " << constIntValue);
  }
}
#endif
//	llvm::CreateGenericValueOfInt(value);
//	int value2 = value.as_float;
*/

namespace floyd {

/*
http://blog.audio-tk.com/2018/09/18/compiling-c-code-in-memory-with-clang/
With LLVM, we also have some things to be careful about. The first is the LLVM context we created before needs to stay alive as long as we use anything from this compilation unit. This is important because everything that is generated with the JIT will have to stay alive after this function and registers itself in the context until it is explicitly deleted.

*/




struct llvmgen_generated_function_t {
	std::vector<global_v_t> local_variable_symbols;
	llvm::Function* f;
};

struct llvmgen_t {
	public: llvmgen_t(llvm_instance_t& instance, const std::string& module_name) :
		instance(&instance),
		module(std::make_unique<llvm::Module>(module_name.c_str(), instance.context)),
		builder(instance.context),
		floyd_runtime_init_f(nullptr)
	{
		QUARK_ASSERT(instance.check_invariant());

	//	llvm::IRBuilder<> builder(instance.context);
		llvm::InitializeNativeTarget();
		llvm::InitializeNativeTargetAsmPrinter();

		QUARK_ASSERT(check_invariant());
	}
	public: bool check_invariant() const {
		QUARK_ASSERT(instance);
		QUARK_ASSERT(instance->check_invariant());
		QUARK_ASSERT(module);

		if(emit_function){
		}
		else{
			//	While emitting a function, the module is in an invalid state.
			QUARK_ASSERT(check_invariant__module(module.get()));
		}

/*
		if(emit_function_incomplete == 0){
			if(emit_function){
				QUARK_ASSERT(check_invariant__function(emit_function->f));
			}
			else{
			}
		}
		else{
		}
*/
		return true;
	}


	llvm_instance_t* instance;

	//	Module must sit in a unique_ptr<> because llvm::EngineBuilder needs that.
	std::unique_ptr<llvm::Module> module;

	llvm::IRBuilder<> builder;

	llvm::Function* floyd_runtime_init_f;

	//	One element for each global symbol in AST. Same indexes as in symbol table.
	std::vector<global_v_t> globals;


	//	0: function is not being modified and should be correct. >0: function is being emitted, can be invalid state.
	std::unique_ptr<llvmgen_generated_function_t> emit_function;
};

void start_function_emit(llvmgen_t& gen_acc, const std::vector<global_v_t>& local_variable_symbols, llvm::Function* stub){
	QUARK_ASSERT(!gen_acc.emit_function);

	auto temp = llvmgen_generated_function_t{local_variable_symbols, stub};
	auto temp2 = std::make_unique<llvmgen_generated_function_t>(temp);
	gen_acc.emit_function.swap(temp2);
}


void end_function_emit(llvmgen_t& gen_acc){
	gen_acc.emit_function.reset();
}



llvm::Value* genllvm_expression(llvmgen_t& gen_acc, const expression_t& e);


/*
std::string print_ee(const llvm::ExecutionEngine& v){
	std::string dump;
	llvm::raw_string_ostream stream2(dump);
	v.print(stream2, nullptr);
	return dump;
}
*/




/// Check a function for errors, useful for use when debugging a
/// pass.
///
/// If there are no errors, the function returns false. If an error is found,
/// a message describing the error is written to OS (if non-null) and true is
/// returned.
//bool verifyFunction(const Function &F, raw_ostream *OS = nullptr);

bool check_invariant__function(const llvm::Function* f){
	QUARK_ASSERT(f != nullptr);

	std::string dump;
	llvm::raw_string_ostream stream2(dump);
	bool errors = llvm::verifyFunction(*f, &stream2);
	if(errors){
		QUARK_TRACE_SS(dump);
	}
	return !errors;
}



bool check_invariant__module(llvm::Module* module){
	QUARK_ASSERT(module != nullptr);

	std::string dump;
	llvm::raw_string_ostream stream2(dump);
	bool module_errors_flag = llvm::verifyModule(*module, &stream2, nullptr);
	if(module_errors_flag){
		QUARK_TRACE_SS(dump);
		QUARK_ASSERT(false);
		return false;
	}

	const auto& functions = module->getFunctionList();
	for(const auto& e: functions){
		QUARK_ASSERT(check_invariant__function(&e));
//		llvm::verifyFunction(e);
	}

	return true;
}



std::string print_module(llvm::Module& module){
	std::string dump;
	llvm::raw_string_ostream stream2(dump);

	stream2 << "\n" "MODULE" << "\n";
	module.print(stream2, nullptr);

/*
	Not needed, module.print() prints the exact list.
	stream2 << "\n" "FUNCTIONS" << "\n";
	const auto& functionList = module.getFunctionList();
	for(const auto& e: functionList){
		e.print(stream2);
	}
*/

	stream2 << "\n" "GLOBALS" << "\n";
	const auto& globalList = module.getGlobalList();
	int index = 0;
	for(const auto& e: globalList){
		stream2 << index << ": ";
		e.print(stream2);
		stream2 << "\n";
		index++;
	}

	return dump;
}

std::string print_program(const llvm_ir_program_t& program){
	QUARK_ASSERT(program.check_invariant());

//	std::string dump;
//	llvm::raw_string_ostream stream2(dump);
//	program.module->print(stream2, nullptr);

	return print_module(*program.module);
}

std::string print_type(llvm::Type* type){
	if(type == nullptr){
		return "nullptr";
	}
	else{
		std::string s;
		llvm::raw_string_ostream rso(s);
		type->print(rso);
//		std::cout<<rso.str();
		return s;
	}
}

std::string print_function(llvm::Function* f){
	if(f == nullptr){
		return "nullptr";
	}
	else{
		QUARK_ASSERT(check_invariant__function(f));

		std::string s;
		llvm::raw_string_ostream rso(s);
		f->print(rso);
//		std::cout<<rso.str();
		return s;
	}
}

std::string print_value0(llvm::Value* value){
	if(value == nullptr){
		return "nullptr";
	}
	else{
		std::string s;
		llvm::raw_string_ostream rso(s);
		value->print(rso);
//		std::cout<<rso.str();
		return s;
	}
}
std::string print_value(llvm::Value* value){
	if(value == nullptr){
		return "nullptr";
	}
	else{
		const std::string type_str = print_type(value->getType());
		const auto val_str = print_value0(value);
		return "[" + type_str + ":" + val_str + "]";
	}
}

std::string print_globals(const std::vector<global_v_t>& globals){
	std::stringstream out;

	out << "{" << std::endl;
	for(const auto& e: globals){
		out << "{ " << print_value(e.value_ptr) << " : " << e.debug_str << " }" << std::endl;
	}

	return out.str();
}



std::string print_gen(const llvmgen_t& gen){
//	QUARK_ASSERT(gen.check_invariant());

	std::stringstream out;

	out << "module:"
		<< print_module(*gen.module)
		<< std::endl
	<< "globals"
		<< print_globals(gen.globals);

	return out.str();
}



global_v_t make_global(llvm::Value* value_ptr, std::string debug_str){
	QUARK_ASSERT(value_ptr != nullptr);

	return { value_ptr, debug_str };
}



/*
First-class values

LLVM has a distinction between first class values and other types of values.
First class values can be returned by instructions, passed to functions,
loaded, stored, PHI'd etc.  Currently the first class value types are:

  1. Integer
  2. Floating point
  3. Pointer
  4. Vector
  5. Opaque (which is assumed to eventually resolve to 1-4)

The non-first-class types are:

  5. Array
  6. Structure/Packed Structure
  7. Function
  8. Void
  9. Label
*/

value_t llvm_to_value(const void* value_ptr, const typeid_t& type){
	QUARK_ASSERT(value_ptr != nullptr);
	QUARK_ASSERT(type.check_invariant());

	//??? more types.
	if(type.is_undefined()){
	}
	else if(type.is_bool()){
		//??? How is int1 encoded by LLVM?
		const auto temp = *static_cast<const uint8_t*>(value_ptr);
		return value_t::make_bool(temp == 0 ? false : true);
	}
	else if(type.is_int()){
		const auto temp = *static_cast<const uint64_t*>(value_ptr);
		return value_t::make_int(temp);
	}
	else if(type.is_double()){
		const auto temp = *static_cast<const double*>(value_ptr);
		return value_t::make_double(temp);
	}
	else if(type.is_string()){
		const uint64_t value = *reinterpret_cast<const uint64_t*>(value_ptr);
		const char* s = reinterpret_cast<const char*>(value);
		return value_t::make_string(s);
	}
	else if(type.is_json_value()){
	}
	else if(type.is_typeid()){
	}
	else if(type.is_struct()){
	}
	else if(type.is_vector()){
	}
	else if(type.is_dict()){
	}
	else if(type.is_function()){
	}
	else{
	}
	QUARK_ASSERT(false);
	throw std::exception();
}






global_v_t find_symbol(llvmgen_t& gen_acc, const variable_address_t& reg){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(reg._parent_steps == -1 || reg._parent_steps == 0);

	if(reg._parent_steps == -1){
		QUARK_ASSERT(reg._index >=0 && reg._index < gen_acc.globals.size());
		return gen_acc.globals[reg._index];
	}
	else if(reg._parent_steps == 0){
		QUARK_ASSERT(gen_acc.emit_function);
		QUARK_ASSERT(reg._index >=0 && reg._index < gen_acc.emit_function->local_variable_symbols.size());

		return gen_acc.emit_function->local_variable_symbols[reg._index];
	}
	else{
		QUARK_ASSERT(false);
	}
}




static const std::string k_expected_ir_fibonacci_text = R"ABC(
; ModuleID = 'fibonacciModule'
source_filename = "fibonacciModule"

define i32 @FibonacciFnc() {
entry:
  %next = alloca i32
  %first = alloca i32
  %second = alloca i32
  %count = alloca i32
  store i32 0, i32* %next
  store i32 0, i32* %first
  store i32 1, i32* %second
  store i32 0, i32* %count
  br label %loopEntry

loopEntry:                                        ; preds = %merge, %entry
  %countVal = load i32, i32* %count
  %enterLoopCond = icmp ult i32 %countVal, 21
  br i1 %enterLoopCond, label %loop, label %exitLoop

loop:                                             ; preds = %loopEntry
  br label %if

exitLoop:                                         ; preds = %loopEntry
  %finalNext = load i32, i32* %next
  ret i32 %finalNext

if:                                               ; preds = %loop
  %ifStmt = icmp ult i32 %countVal, 2
  br i1 %ifStmt, label %ifTrue, label %else

ifTrue:                                           ; preds = %if
  %nextVal = load i32, i32* %count
  store i32 %nextVal, i32* %next
  br label %merge

else:                                             ; preds = %if
  %firstVal = load i32, i32* %first
  %secondVal = load i32, i32* %second
  %nextVal1 = add i32 %firstVal, %secondVal
  store i32 %nextVal1, i32* %next
  store i32 %secondVal, i32* %first
  store i32 %nextVal1, i32* %second
  br label %merge

merge:                                            ; preds = %else, %ifTrue
  %0 = add i32 %countVal, 1
  store i32 %0, i32* %count
  br label %loopEntry
}
)ABC";



static llvm::Function* InitFibonacciFnc(llvm::LLVMContext& context, std::unique_ptr<llvm::Module> module, llvm::IRBuilder<>& builder, int targetFibNum){
	llvm::Function* f = llvm::cast<llvm::Function>(
		module->getOrInsertFunction("FibonacciFnc", llvm::Type::getInt32Ty(context))
	);

	llvm::Value* zero = llvm::ConstantInt::get(builder.getInt32Ty(), 0);
	llvm::Value* one = llvm::ConstantInt::get(builder.getInt32Ty(), 1);
	llvm::Value* two = llvm::ConstantInt::get(builder.getInt32Ty(), 2);
	llvm::Value* N = llvm::ConstantInt::get(builder.getInt32Ty(), targetFibNum);


	////////////////////////		Create all basic blocks first, so we can branch between them when we start emitting instructions

	llvm::BasicBlock* EntryBB = llvm::BasicBlock::Create(context, "entry", f);
	llvm::BasicBlock* LoopEntryBB = llvm::BasicBlock::Create(context, "loopEntry", f);

	llvm::BasicBlock* LoopBB = llvm::BasicBlock::Create(context, "loop", f);
		llvm::BasicBlock* IfBB = llvm::BasicBlock::Create(context, "if"); 			//floating
		llvm::BasicBlock* ThenBB = llvm::BasicBlock::Create(context, "ifTrue"); 	//floating
		llvm::BasicBlock* ElseBB = llvm::BasicBlock::Create(context, "else"); 		//floating
		llvm::BasicBlock* MergeBB = llvm::BasicBlock::Create(context, "merge"); 	//floating
	llvm::BasicBlock* ExitLoopBB = llvm::BasicBlock::Create(context, "exitLoop", f);


	////////////////////////		EntryBB

	builder.SetInsertPoint(EntryBB);
	llvm::Value* next = builder.CreateAlloca(llvm::Type::getInt32Ty(context), nullptr, "next");
	llvm::Value* first = builder.CreateAlloca(llvm::Type::getInt32Ty(context), nullptr, "first");
	llvm::Value* second = builder.CreateAlloca(llvm::Type::getInt32Ty(context), nullptr, "second");
	llvm::Value* count = builder.CreateAlloca(llvm::Type::getInt32Ty(context), nullptr, "count");
	builder.CreateStore(zero, next);
	builder.CreateStore(zero, first);
	builder.CreateStore(one, second);
	builder.CreateStore(zero, count);

	// continue to loop entry
	builder.CreateBr(LoopEntryBB);


	////////////////////////		LoopEntryBB

	builder.SetInsertPoint(LoopEntryBB);
	llvm::Value* countVal = builder.CreateLoad(count, "countVal");
	llvm::Value* ifCountLTN = builder.CreateICmpULT(countVal, N, "enterLoopCond");
	builder.CreateCondBr(ifCountLTN, LoopBB, ExitLoopBB);


	////////////////////////		LoopBB

	builder.SetInsertPoint(LoopBB);
	builder.CreateBr(IfBB);


		////////////////////////		IfBB

		// Nested statements are attached just before adding to the block, so that
		// their insertion point in LoopBB is certain.
		f->getBasicBlockList().push_back(IfBB);
		builder.SetInsertPoint(IfBB);
		llvm::Value* ifCountLTTwo = builder.CreateICmpULT(countVal, two, "ifStmt");
		builder.CreateCondBr(ifCountLTTwo, ThenBB, ElseBB);

		////////////////////////		ThenBB

		f->getBasicBlockList().push_back(ThenBB);
		builder.SetInsertPoint(ThenBB);
		llvm::Value* nextVal = builder.CreateLoad(count, "nextVal");
		builder.CreateStore(nextVal, next);

		// terminate ThenBB
		builder.CreateBr(MergeBB);


		////////////////////////		ElseBB

		f->getBasicBlockList().push_back(ElseBB);
		builder.SetInsertPoint(ElseBB);
	
		llvm::Value* firstVal = builder.CreateLoad(first, "firstVal");
		llvm::Value* secondVal = builder.CreateLoad(second, "secondVal");
		nextVal = builder.CreateAdd(firstVal, secondVal, "nextVal");
		builder.CreateStore(nextVal, next);
		builder.CreateStore(secondVal, first);
		builder.CreateStore(nextVal, second);

		// terminate ElseBB
		builder.CreateBr(MergeBB);


		////////////////////////		MergeBB

		f->getBasicBlockList().push_back(MergeBB);
		builder.SetInsertPoint(MergeBB);
		countVal = builder.CreateAdd(countVal, one); //increment
		builder.CreateStore(countVal, count);
		builder.CreateBr(LoopEntryBB);


	////////////////////////		ExitLoopBB

	builder.SetInsertPoint(ExitLoopBB);
	llvm::Value* finalFibNum = builder.CreateLoad(next, "finalNext");
	llvm::ReturnInst::Create(context, finalFibNum, ExitLoopBB);

	return f;
}


enum class func_encode {
	functions_are_values,
	functions_are_pointers
};

llvm::Type* intern_type(llvmgen_t& gen_acc, const typeid_t& type, func_encode encode){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(type.check_invariant());

	auto& context = gen_acc.instance->context;

	if(type.is_void()){
		return llvm::Type::getVoidTy(context);
	}
	else if(type.is_int()){
		return llvm::Type::getInt64Ty(context);
	}
	else if(type.is_bool()){
		return llvm::Type::getInt1Ty(context);
	}

	else if(type.is_string()){
		return llvm::Type::getInt8PtrTy(context);
	}
	else if(type.is_json_value()){
		return llvm::Type::getIntNTy(context, 16);
	}
	else if(type.is_vector()){
		return llvm::Type::getIntNTy(context, 16);
	}
	else if(type.is_typeid()){
		return llvm::Type::getIntNTy(context, 16);
	}
	else if(type.is_undefined()){
		return llvm::Type::getIntNTy(context, 16);
	}
	else if(type.is_unresolved_type_identifier()){
		QUARK_ASSERT(false);
		return llvm::Type::getIntNTy(context, 16);
	}
	else if(type.is_double()){
		return llvm::Type::getDoubleTy(context);
	}
	else if(type.is_struct()){
		return llvm::Type::getIntNTy(context, 16);

#if 0
		std::vector<llvm::Type*> members;
		for(const auto& m: type.get_struct_ref()->_members){
			const auto m2 = intern_type(gen_acc, m._type, encode);
			members.push_back(m2);
		}

  		llvm::StructType* s = llvm::StructType::get(context, members, false);

//		return llvm::StructType::get(context);
//		return llvm::Type::getInt32Ty(context);
		return s;
#endif

	}

	else if(type.is_internal_dynamic()){
		//	Use int16ptr as placeholder for **dyn**. Maybe pass a struct instead?
		return llvm::Type::getIntNTy(context, 16);
	}
	else if(type.is_function()){
		const auto& return_type = type.get_function_return();
		const auto args = type.get_function_args();

		const auto return_type2 = intern_type(gen_acc, return_type, encode);

		// Make the function type:  double(double,double) etc.
		std::vector<llvm::Type*> args2;

		//	Pass Floyd runtime as extra, hidden argument #0.
		llvm::Type* context_ptr = llvm::Type::getInt32PtrTy(context);
		args2.push_back(context_ptr);

		for(const auto& arg: args){
			auto arg_type = intern_type(gen_acc, arg, encode);
			args2.push_back(arg_type);
		}

		llvm::FunctionType* function_type = llvm::FunctionType::get(return_type2, args2, false);

		if(encode == func_encode::functions_are_values){
			return function_type;
		}
		else if(encode == func_encode::functions_are_pointers){
			auto function_pointer_type = function_type->getPointerTo();
			return function_pointer_type;
		}
		else{
			QUARK_ASSERT(false);
		}

	}
	else{
		QUARK_TRACE_SS(floyd::typeid_to_compact_string(type));
		QUARK_ASSERT(false);
		quark::throw_exception();
	}
}

//mJit->addGlobalMapping(fn_func, reinterpret_cast<void*>(myFunc));

//	??? temp workaround -- we don't support all argument types yet.
typeid_t hack_function_type(const typeid_t& function_type){
	QUARK_ASSERT(function_type.check_invariant());

	floyd::typeid_t hacked_function_type = floyd::typeid_t::make_function(
		function_type.get_function_return(),
		{
			floyd::typeid_t::make_int()
		},
		floyd::epure::impure
	);
	return hacked_function_type;
}

//	Function prototype must NOT EXIST already.
llvm::Function* make_function_stub(llvmgen_t& gen_acc, const std::string& function_name, const floyd::typeid_t& function_type){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(function_type.check_invariant());
	auto existing_f = gen_acc.module->getFunction(function_name);
	QUARK_ASSERT(existing_f == nullptr);

	floyd::typeid_t hacked_function_type = hack_function_type(function_type);

	llvm::Type* ftype = intern_type(gen_acc, hacked_function_type, func_encode::functions_are_values);

	//	IMPORTANT: Must cast to (llvm::FunctionType*) to get correct overload of getOrInsertFunction() to be called!
	auto f3 = gen_acc.module->getOrInsertFunction(function_name, (llvm::FunctionType*)ftype);
	llvm::Function* f = llvm::cast<llvm::Function>(f3);

	QUARK_ASSERT(check_invariant__function(f));
	QUARK_ASSERT(gen_acc.check_invariant());
	return f;
}

std::string make_unique_internal_function_name2(int function_id){
	const auto s = std::string() + "floyd_internal_" + std::to_string(function_id);
	return s;
}
std::string make_unique_internal_function_name(const value_t& function_value){
	QUARK_ASSERT(function_value.check_invariant());

	const auto function_id = function_value.get_function_value();
	return make_unique_internal_function_name2(function_id);
}
std::string make_host_function_label(int host_function_id){
//	const auto s = "floyd_host_function_ABC";
	const auto s = std::string() + "floyd_host_function_" + std::to_string(host_function_id);
	return s;
}





#if 0
std::vector<llvm::Constant*> values;
...
/* Make the value 42 appear in the array - ty is "i32" */
llvm::Constant* c = llvm::Constant::getIntegerValue(ty, 42);
values.push_back(c);
... // Add more values here ...
llvm::Constant* init = llvm::ConstantArray::get(arrayTy_0, values);
GArray->setInitializer(init);
#endif

llvm::Constant* make_constant(llvmgen_t& gen_acc, const value_t& value){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(value.check_invariant());

	auto& context = gen_acc.instance->context;
	const auto type = value.get_type();
	const auto itype = intern_type(gen_acc, type, func_encode::functions_are_pointers);

	if(type.is_undefined()){
		return llvm::ConstantInt::get(itype, 17);
	}
	else if(type.is_internal_dynamic()){
		return llvm::ConstantInt::get(itype, 13);
	}
	else if(type.is_void()){
		QUARK_ASSERT(false);
		return nullptr;
	}
	else if(type.is_bool()){
		return llvm::ConstantInt::get(itype, value.get_bool_value() ? 1 : 0);
	}
	else if(type.is_int()){
		return llvm::ConstantInt::get(itype, value.get_int_value());
	}
	else if(type.is_double()){
		return llvm::ConstantFP::get(itype, value.get_double_value());
	}
	else if(type.is_string()){
		//	Stores trailing zero. ??? not pure string!
//		llvm::Constant* array = llvm::ConstantDataArray::getString(context, value.get_string_value(), true);

		// The type of your string will be [n x i8], it needs to be i8*, so we cast here. We
		// explicitly use the type of printf's first arg to guarantee we are always right.

		llvm::PointerType* int8Ptr_type = llvm::Type::getInt8PtrTy(context);

		llvm::Constant* c2 = gen_acc.builder.CreateGlobalStringPtr(value.get_string_value());
		//	, "cast [n x i8] to i8*"
//		llvm::Constant* c = gen_acc.builder.CreatePointerCast(array, int8Ptr_type);
		return c2;

//		return gen_acc.builder.CreateGlobalStringPtr(llvm::StringRef(value.get_string_value()));
	}
	else if(type.is_json_value()){
		return llvm::ConstantInt::get(itype, 7000);
	}
	else if(type.is_typeid()){
		return llvm::ConstantInt::get(itype, 6000);
	}
	else if(type.is_struct()){
		QUARK_ASSERT(false);
		return nullptr;
	}
	else if(type.is_protocol()){
		QUARK_ASSERT(false);
		return nullptr;
	}
	else if(type.is_vector()){
		QUARK_ASSERT(false);
		return nullptr;
	}
	else if(type.is_dict()){
		QUARK_ASSERT(false);
		return nullptr;
	}
	else if(type.is_function()){
		const auto unique_function_str = make_unique_internal_function_name(value);
		return make_function_stub(gen_acc, unique_function_str, hack_function_type(type));
	}
	else if(type.is_unresolved_type_identifier()){
		QUARK_ASSERT(false);
		return nullptr;
	}
	else{
		QUARK_ASSERT(false);
		return nullptr;
	}
}


llvm::Value* genllvm_literal_expression(llvmgen_t& gen_acc, const expression_t& e){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(e.check_invariant());

	const auto literal = e.get_literal();
	return make_constant(gen_acc, literal);
}

llvm::Value* genllvm_arithmetic_expression(llvmgen_t& gen_acc, expression_type op, const expression_t& e){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(e.check_invariant());

	const auto type = e._input_exprs[0].get_output_type();

	auto lhs_temp = genllvm_expression(gen_acc, e._input_exprs[0]);
	auto rhs_temp = genllvm_expression(gen_acc, e._input_exprs[1]);

/*
	if(type.is_bool()){
		static const std::map<expression_type, bc_opcode> conv_opcode = {
			{ expression_type::k_arithmetic_add__2, bc_opcode::k_add_bool },
			{ expression_type::k_arithmetic_subtract__2, bc_opcode::k_nop },
			{ expression_type::k_arithmetic_multiply__2, bc_opcode::k_nop },
			{ expression_type::k_arithmetic_divide__2, bc_opcode::k_nop },
			{ expression_type::k_arithmetic_remainder__2, bc_opcode::k_nop },

			{ expression_type::k_logical_and__2, bc_opcode::k_logical_and_bool },
			{ expression_type::k_logical_or__2, bc_opcode::k_logical_or_bool }
		};
		return conv_opcode.at(e._operation);
	}
	else*/
	if(type.is_int()){
		if(e._operation == expression_type::k_arithmetic_add__2){
			return gen_acc.builder.CreateAdd(lhs_temp, rhs_temp, "add_tmp");
		}
		else if(e._operation == expression_type::k_arithmetic_subtract__2){
			return gen_acc.builder.CreateSub(lhs_temp, rhs_temp, "subtract_tmp");
		}
		else if(e._operation == expression_type::k_arithmetic_multiply__2){
			return gen_acc.builder.CreateMul(lhs_temp, rhs_temp, "mult_tmp");
		}
		else if(e._operation == expression_type::k_arithmetic_divide__2){
			return gen_acc.builder.CreateSDiv(lhs_temp, rhs_temp, "divide_tmp");
		}
		else if(e._operation == expression_type::k_arithmetic_remainder__2){
			return gen_acc.builder.CreateSRem(lhs_temp, rhs_temp, "reminder_tmp");
		}

		else if(e._operation == expression_type::k_logical_and__2){
			QUARK_ASSERT(false);
			quark::throw_exception();
			return gen_acc.builder.CreateAnd(lhs_temp, rhs_temp, "logical_and_tmp");
		}
		else if(e._operation == expression_type::k_logical_or__2){
			QUARK_ASSERT(false);
			quark::throw_exception();
			return gen_acc.builder.CreateOr(lhs_temp, rhs_temp, "logical_or_tmp");
		}
		else{
			QUARK_ASSERT(false);
		}
	}
	else if(type.is_double()){
		if(e._operation == expression_type::k_arithmetic_add__2){
			return gen_acc.builder.CreateFAdd(lhs_temp, rhs_temp, "add_tmp");
		}
		else if(e._operation == expression_type::k_arithmetic_subtract__2){
			return gen_acc.builder.CreateFSub(lhs_temp, rhs_temp, "subtract_tmp");
		}
		else if(e._operation == expression_type::k_arithmetic_multiply__2){
			return gen_acc.builder.CreateFMul(lhs_temp, rhs_temp, "mult_tmp");
		}
		else if(e._operation == expression_type::k_arithmetic_divide__2){
			return gen_acc.builder.CreateFDiv(lhs_temp, rhs_temp, "divide_tmp");
		}
		else if(e._operation == expression_type::k_arithmetic_remainder__2){
			QUARK_ASSERT(false);
			quark::throw_exception();
		}
		else if(e._operation == expression_type::k_logical_and__2){
			QUARK_ASSERT(false);
			quark::throw_exception();
		}
		else if(e._operation == expression_type::k_logical_or__2){
			QUARK_ASSERT(false);
			quark::throw_exception();
		}
		else{
			QUARK_ASSERT(false);
		}
	}
/*

	else if(type.is_string()){
		static const std::map<expression_type, bc_opcode> conv_opcode = {
			{ expression_type::k_arithmetic_add__2, bc_opcode::k_concat_strings },
			{ expression_type::k_arithmetic_subtract__2, bc_opcode::k_nop },
			{ expression_type::k_arithmetic_multiply__2, bc_opcode::k_nop },
			{ expression_type::k_arithmetic_divide__2, bc_opcode::k_nop },
			{ expression_type::k_arithmetic_remainder__2, bc_opcode::k_nop },

			{ expression_type::k_logical_and__2, bc_opcode::k_nop },
			{ expression_type::k_logical_or__2, bc_opcode::k_nop }
		};
		return conv_opcode.at(e._operation);
	}
	else if(type.is_vector()){
		if(encode_as_vector_w_inplace_elements(type)){
			static const std::map<expression_type, bc_opcode> conv_opcode = {
				{ expression_type::k_arithmetic_add__2, bc_opcode::k_concat_vectors_w_inplace_elements },
				{ expression_type::k_arithmetic_subtract__2, bc_opcode::k_nop },
				{ expression_type::k_arithmetic_multiply__2, bc_opcode::k_nop },
				{ expression_type::k_arithmetic_divide__2, bc_opcode::k_nop },
				{ expression_type::k_arithmetic_remainder__2, bc_opcode::k_nop },

				{ expression_type::k_logical_and__2, bc_opcode::k_nop },
				{ expression_type::k_logical_or__2, bc_opcode::k_nop }
			};
			return conv_opcode.at(e._operation);
		}
		else{
			static const std::map<expression_type, bc_opcode> conv_opcode = {
				{ expression_type::k_arithmetic_add__2, bc_opcode::k_concat_vectors_w_external_elements },
				{ expression_type::k_arithmetic_subtract__2, bc_opcode::k_nop },
				{ expression_type::k_arithmetic_multiply__2, bc_opcode::k_nop },
				{ expression_type::k_arithmetic_divide__2, bc_opcode::k_nop },
				{ expression_type::k_arithmetic_remainder__2, bc_opcode::k_nop },

				{ expression_type::k_logical_and__2, bc_opcode::k_nop },
				{ expression_type::k_logical_or__2, bc_opcode::k_nop }
			};
			return conv_opcode.at(e._operation);
		}
	}
*/
	QUARK_ASSERT(false);
	quark::throw_exception();

	return nullptr;
}

llvm::Value* genllvm_arithmetic_unary_minus_expression(llvmgen_t& gen_acc, const expression_t& e){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(e.check_invariant());

	const auto type = e._input_exprs[0].get_output_type();
	if(type.is_int()){
		const auto e2 = expression_t::make_simple_expression__2(expression_type::k_arithmetic_subtract__2, expression_t::make_literal_int(0), e._input_exprs[0], e._output_type);
		return genllvm_expression(gen_acc, e2);
	}
	else if(type.is_double()){
		const auto e2 = expression_t::make_simple_expression__2(expression_type::k_arithmetic_subtract__2, expression_t::make_literal_double(0), e._input_exprs[0], e._output_type);
		return genllvm_expression(gen_acc, e2);
	}
	else{
		QUARK_ASSERT(false);
		quark::throw_exception();
	}
}





/*
	my_func
		[BB entry]
			...
		[BB xyz]
			condition_value = expr[0]
			if condition_value %then, %else

		[BB then]
			[BB ...]
				[BB ...]
					[BB ...]
						[BB ...]
				[BB ...]
					[BB ...]
			temp1 = expr[1]
			br %join
		[BB else]
			[BB ...]
				[BB ...]
					[BB ...]
						[BB ...]
				[BB ...]
					[BB ...]
			temp2 = expr[2]
			br %join
		[BB join]
 			Value* phu(temp1, temp2)
*/



llvm::Value* llvmgen_conditional_operator_expression(llvmgen_t& gen_acc, const expression_t& e){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(e.check_invariant());

	auto& context = gen_acc.instance->context;
	auto& builder = gen_acc.builder;

	const auto result_type = e.get_output_type();
	const auto result_itype = intern_type(gen_acc, result_type, func_encode::functions_are_pointers);

	llvm::Value* condition_value = genllvm_expression(gen_acc, e._input_exprs[0]);

	llvm::Function* parent_function = builder.GetInsertBlock()->getParent();

	// Create blocks for the then and else cases.  Insert the 'then' block at the
	// end of the function.
	llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(context, "then", parent_function);
	llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(context, "else");
	llvm::BasicBlock* join_bb = llvm::BasicBlock::Create(context, "cond_operator-join");

	builder.CreateCondBr(condition_value, then_bb, else_bb);

	// Emit then-value.
	builder.SetInsertPoint(then_bb);
	llvm::Value* then_value = genllvm_expression(gen_acc, e._input_exprs[1]);
	builder.CreateBr(join_bb);
	// Codegen of 'Then' can change the current block, update then_bb.
	llvm::BasicBlock* then_bb2 = builder.GetInsertBlock();

	// Emit else block.
	parent_function->getBasicBlockList().push_back(else_bb);
	builder.SetInsertPoint(else_bb);
	llvm::Value* else_value = genllvm_expression(gen_acc, e._input_exprs[2]);
	builder.CreateBr(join_bb);
	// Codegen of 'Else' can change the current block, update else_bb.
	llvm::BasicBlock* else_bb2 = builder.GetInsertBlock();

	// Emit join block.
	parent_function->getBasicBlockList().push_back(join_bb);
	builder.SetInsertPoint(join_bb);
	llvm::PHINode* phiNode = builder.CreatePHI(result_itype, 2, "cond_operator-result");

	phiNode->addIncoming(then_value, then_bb2);
	phiNode->addIncoming(else_value, else_bb2);

	return phiNode;
}



////////////////////////////////	Host functions, automatically linked into the LLVM execution engine.

/*
@variable = global i32 21
define i32 @main() {
%1 = load i32, i32* @variable ; load the global variable
%2 = mul i32 %1, 2
store i32 %2, i32* @variable ; store instruction to write to global variable
ret i32 %2
}
*/


llvm_execution_engine_t* get_floyd_runtime(void* floyd_runtime_ptr){
	QUARK_ASSERT(floyd_runtime_ptr != nullptr);

	auto ptr = reinterpret_cast<llvm_execution_engine_t*>(floyd_runtime_ptr);
	QUARK_ASSERT(ptr != nullptr);
	QUARK_ASSERT(ptr->debug_magic == k_debug_magic);
	return ptr;
}

void hook(const std::string& s, void* floyd_runtime_ptr, int64_t arg){
	std:: cout << s << arg << " arg: " << std::endl;
	auto r = get_floyd_runtime(floyd_runtime_ptr);
}

//	The names of these are computed from the host-id in the symbol table, not the names of the functions/symbols.
//	They must use C calling convention so llvm JIT can find them.
//	Make sure they are not dead-stripped out of binary!
extern "C" {

	extern void floyd_host_function_1000(void* floyd_runtime_ptr, int64_t arg){
		std:: cout << "floyd_host_function_1000: " << arg << std::endl;

		auto r = get_floyd_runtime(floyd_runtime_ptr);

		const auto s = std::to_string(arg);
		r->_print_output.push_back(s);
	}

	extern void floyd_host_function_1001(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1002(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1003(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1004(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1005(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1006(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1007(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1008(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1009(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}



	extern void floyd_host_function_1010(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1011(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1012(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1013(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1014(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1015(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1016(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1017(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1018(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1019(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}



	extern void floyd_host_function_1020(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1021(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1022(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1023(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1024(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1025(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1026(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1027(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1028(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1029(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}




	extern void floyd_host_function_1030(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1031(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1032(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1033(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1034(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1035(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1036(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1037(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1038(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}

	extern void floyd_host_function_1039(void* floyd_runtime_ptr, int64_t arg){
		hook(__FUNCTION__, floyd_runtime_ptr, arg);
	}




	///////////////		TEST

	extern void floyd_host_function_2002(void* floyd_runtime_ptr, int64_t arg){
		std:: cout << __FUNCTION__ << arg << std::endl;
	}

	extern void floyd_host_function_2003(void* floyd_runtime_ptr, int64_t arg){
		std:: cout << __FUNCTION__ << arg << std::endl;
	}

}


llvm::Value* llvmgen_call_expression(llvmgen_t& gen_acc, const expression_t& e){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(e.check_invariant());

	//	[0] is the callee, which is required. [1] etc are the args, which are optional.
	QUARK_ASSERT(e._input_exprs.size() >= 1);

	//	_input_exprs[0] is callee, rest are arguments.
	const auto callee_arg_count = static_cast<int>(e._input_exprs.size()) - 1;

	const auto function_type = e._input_exprs[0].get_output_type();
	const auto function_def_arg_types = function_type.get_function_args();
	QUARK_ASSERT(callee_arg_count == function_def_arg_types.size());
	const auto return_type = e.get_output_type();

	QUARK_ASSERT(callee_arg_count == function_def_arg_types.size());
	const auto arg_count = callee_arg_count;

//	int host_function_id = get_host_function_id(gen_acc, e);

//	floyd::typeid_t print_function_type = floyd::typeid_t::make_function(typeid_t::make_void(), { floyd::typeid_t::make_int() }, floyd::epure::impure);


	//	Normal function call.
	{
		llvm::Value* callee0 = genllvm_expression(gen_acc, e._input_exprs[0]);
		QUARK_TRACE_SS("callee0: " << print_value(callee0));

//		QUARK_TRACE_SS("gen_acc: " << print_gen(gen_acc));

		std::vector<llvm::Value*> args2;

		auto& emit_function = *gen_acc.emit_function;

		//	Insert floyd_runtime_ptr as first argument to called function.
		//	Function::ArgumentListType &getArgumentList()
		auto args = emit_function.f->args();
		QUARK_ASSERT((args.end() - args.begin()) >= 1);
		auto floyd_context_arg_ptr = args.begin();
//		callee_f->args();
		args2.push_back(floyd_context_arg_ptr);


		//	Skip input argument [0], which is callee.
		for(int i = 1 ; i < e._input_exprs.size() ; i++){
			llvm::Value* arg2 = genllvm_expression(gen_acc, e._input_exprs[i]);
			args2.push_back(arg2);
		}

		if(return_type.is_void()){
			auto result = gen_acc.builder.CreateCall(callee0, args2, "");
			QUARK_ASSERT(gen_acc.check_invariant());
			return result;
		}
		else{
			auto result = gen_acc.builder.CreateCall(callee0, args2, "temp_call");
			QUARK_ASSERT(gen_acc.check_invariant());
			return result;
		}
	}
}

llvm::Value* llvmgen_load2_expression(llvmgen_t& gen_acc, const expression_t& e){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(e.check_invariant());

//	QUARK_TRACE_SS("result = " << floyd::print_program(gen_acc.program_acc));

	auto dest = find_symbol(gen_acc, e._address);
	return gen_acc.builder.CreateLoad(dest.value_ptr);
}

//??? use visitor and std::variant<>
llvm::Value* genllvm_expression(llvmgen_t& gen_acc, const expression_t& e){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(e.check_invariant());

	const auto op = e.get_operation();
	if(op == expression_type::k_literal){
		return genllvm_literal_expression(gen_acc, e);
	}
	else if(op == expression_type::k_resolve_member){
		QUARK_ASSERT(false);
		quark::throw_exception();
//		return bcgen_resolve_member_expression(gen_acc, target_reg, e, body);
	}
	else if(op == expression_type::k_lookup_element){
		QUARK_ASSERT(false);
		quark::throw_exception();
//		return bcgen_lookup_element_expression(gen_acc, target_reg, e, body);
	}
	else if(op == expression_type::k_load2){
#if 0
		if(e.get_output_type().is_function()){
			llvm::Value* dest = find_symbol(gen_acc, gen_acc.globals, e._address);
			return dest;
		}
		else{
			return llvmgen_load2_expression(gen_acc, e, func_acc);
		}
#else
		return llvmgen_load2_expression(gen_acc, e);
#endif
	}
	else if(op == expression_type::k_call){
		return llvmgen_call_expression(gen_acc, e);
	}
	else if(op == expression_type::k_value_constructor){
		QUARK_ASSERT(false);
		quark::throw_exception();
//		return bcgen_construct_value_expression(gen_acc, target_reg, e, body);
	}
	else if(op == expression_type::k_arithmetic_unary_minus__1){
		return genllvm_arithmetic_unary_minus_expression(gen_acc, e);
	}
	else if(op == expression_type::k_conditional_operator3){
		return llvmgen_conditional_operator_expression(gen_acc, e);
	}
	else if (is_arithmetic_expression(op)){
		return genllvm_arithmetic_expression(gen_acc, op, e);
	}
	else if (is_comparison_expression(op)){
		QUARK_ASSERT(false);
		quark::throw_exception();
//		return bcgen_comparison_expression(gen_acc, target_reg, op, e, body);
	}
	else{
		QUARK_ASSERT(false);
	}
	quark::throw_exception();
}



void genllvm_store2_statement(llvmgen_t& gen_acc, const statement_t::store2_t& s){
	QUARK_ASSERT(gen_acc.check_invariant());

	llvm::Value* value = genllvm_expression(gen_acc, s._expression);

	auto dest = find_symbol(gen_acc, s._dest_variable);
	gen_acc.builder.CreateStore(value, dest.value_ptr);

	QUARK_ASSERT(gen_acc.check_invariant());
}

void genllvm_expression_statement(llvmgen_t& gen_acc, const statement_t::expression_statement_t& s){
	QUARK_ASSERT(gen_acc.check_invariant());

	genllvm_expression(gen_acc, s._expression);

	QUARK_ASSERT(gen_acc.check_invariant());
}

/*
	All Floyd's global statements becomes instructions in floyd_init()-function that is called by Floyd runtime before any other function is called.
*/

void genllvm_statements(llvmgen_t& gen_acc, const std::vector<statement_t>& statements){
	QUARK_ASSERT(gen_acc.check_invariant());

	if(statements.empty() == false){
		for(const auto& statement: statements){
			QUARK_ASSERT(statement.check_invariant());

			struct visitor_t {
				llvmgen_t& acc0;

				void operator()(const statement_t::return_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
//					return bcgen_return_statement(_gen_acc, s, body_acc);
				}
				void operator()(const statement_t::define_struct_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
				}
				void operator()(const statement_t::define_protocol_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
				}
				void operator()(const statement_t::define_function_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
				}

				void operator()(const statement_t::bind_local_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
				}
				void operator()(const statement_t::store_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
				}
				void operator()(const statement_t::store2_t& s) const{
					genllvm_store2_statement(acc0, s);
				}
				void operator()(const statement_t::block_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
//					return bcgen_block_statement(_gen_acc, s, body_acc);
				}

				void operator()(const statement_t::ifelse_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
//					return bcgen_ifelse_statement(_gen_acc, s, body_acc);
				}
				void operator()(const statement_t::for_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
//					return bcgen_for_statement(_gen_acc, s, body_acc);
				}
				void operator()(const statement_t::while_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
//					return bcgen_while_statement(_gen_acc, s, body_acc);
				}


				void operator()(const statement_t::expression_statement_t& s) const{
					genllvm_expression_statement(acc0, s);
				}
				void operator()(const statement_t::software_system_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
//					return body_acc;
				}
				void operator()(const statement_t::container_def_statement_t& s) const{
					QUARK_ASSERT(false);
					quark::throw_exception();
//					return body_acc;
				}
			};

			std::visit(visitor_t{ gen_acc }, statement._contents);
		}
	}
}

std::vector<global_v_t> genllvm_local_make_symbols(llvmgen_t& gen_acc, const symbol_table_t& symbol_table){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(symbol_table.check_invariant());

	std::vector<global_v_t> result;

	for(const auto& e: symbol_table._symbols){
		const auto type = e.second.get_type();
		const auto itype = intern_type(gen_acc, type, func_encode::functions_are_pointers);

		llvm::Value* dest = gen_acc.builder.CreateAlloca(itype, nullptr, e.first);

		if(e.second._init.is_undefined() == false){
			llvm::Value* c = make_constant(gen_acc, e.second._init);
			gen_acc.builder.CreateStore(c, dest);
		}
		else{
		}
		const auto debug_str = "name:" + e.first + " symbol_t: " + symbol_to_string(e.second);
		result.push_back(make_global(dest, debug_str));
	}
	return result;
}



llvm::Value* genllvm_make_global(llvmgen_t& gen_acc, const semantic_ast_t& ast, const std::string& symbol_name, const symbol_t& symbol){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(ast.check_invariant());
	QUARK_ASSERT(symbol_name.empty() == false);
	QUARK_ASSERT(symbol.check_invariant());

	const auto type0 = symbol.get_type();

	if(type0.is_function()){
		const auto itype = intern_type(gen_acc, hack_function_type(type0), func_encode::functions_are_pointers);
//		QUARK_TRACE_SS("itype: " << print_type(itype));

		const int function_id = symbol._init.get_function_value();
		QUARK_ASSERT(function_id >= 0 && function_id < ast._tree._function_defs.size());
		const auto function_def0 = ast._tree._function_defs[function_id];
		if(function_def0 != nullptr){
			const auto function_def = *function_def0;
			if(function_def._host_function_id != k_no_host_function_id){
				const auto label = make_host_function_label(function_def._host_function_id);
				llvm::Function* f = gen_acc.module->getFunction(label);
				QUARK_ASSERT(f != nullptr);

				llvm::Value* gv = new llvm::GlobalVariable(
					*gen_acc.module,
					itype,
					true,
					llvm::GlobalValue::ExternalLinkage,
					f,
					symbol_name
				);

//				QUARK_TRACE_SS("global value: " << print_value(gv));

				return gv;
			}
			else{
				return nullptr;
			}
		}
		else{
			return nullptr;
		}
	}
	else{
		const auto itype = intern_type(gen_acc, type0, func_encode::functions_are_pointers);
		llvm::Value* init = nullptr;
		if(symbol._init.is_undefined() == false){
			llvm::Value* c = make_constant(gen_acc, symbol._init);
	//				dest->setInitializer(constant_value);
			init = c;
		}

		llvm::Value* gv = new llvm::GlobalVariable(
			*gen_acc.module,
			itype,
			true,
			llvm::GlobalValue::ExternalLinkage,
	//			init,
			llvm::Constant::getNullValue(itype),
			symbol_name
		);

		return gv;
	}
}



std::vector<global_v_t> genllvm_make_globals(llvmgen_t& gen_acc, const semantic_ast_t& ast, const symbol_table_t& symbol_table){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(ast.check_invariant());
	QUARK_ASSERT(symbol_table.check_invariant());

//	QUARK_TRACE_SS("result = " << floyd::print_program(gen_acc.program_acc));

	std::vector<global_v_t> result;

	for(const auto& e: symbol_table._symbols){
		llvm::Value* value = genllvm_make_global(gen_acc, ast, e.first, e.second);
		const auto debug_str = "name:" + e.first + " symbol_t: " + symbol_to_string(e.second);
		result.push_back(make_global(value, debug_str));

//		QUARK_TRACE_SS("result = " << floyd::print_program(gen_acc.program_acc));
	}
	return result;
}



void genllvm_function_def(llvmgen_t& gen_acc, const floyd::function_definition_t& function_def){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(function_def.check_invariant());

	llvm::Function* f = make_function_stub(gen_acc, "generated_func_name", function_def._function_type);
	QUARK_ASSERT(check_invariant__function(f));

	llvm::BasicBlock* entryBB = llvm::BasicBlock::Create(gen_acc.instance->context, "entry", f);
	gen_acc.builder.SetInsertPoint(entryBB);

	auto symbol_table_values = genllvm_local_make_symbols(gen_acc, function_def._body->_symbol_table);

	{
		start_function_emit(gen_acc, symbol_table_values, f);
		genllvm_statements(gen_acc, function_def._body->_statements);
		end_function_emit(gen_acc);
	}

	QUARK_ASSERT(check_invariant__function(f));
}

void genllvm_make_function_defs(llvmgen_t& gen_acc, const semantic_ast_t& semantic_ast){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(semantic_ast.check_invariant());

	//	We already generate the LLVM function-prototypes for the global functions in genllvm_all().
	for(int function_id = 0 ; function_id < semantic_ast._tree._function_defs.size() ; function_id++){
		const auto& function_def = *semantic_ast._tree._function_defs[function_id];

		if(function_def._host_function_id != k_no_host_function_id){
		}
		else{
			QUARK_ASSERT(false);
//???			genllvm_function_def(gen_acc, function_def);
		}
	}
}

//	Generate floyd_runtime_init() that runs all global statements, before main() is run.
//	The AST contains statements that initializes the global variables, including global functions.
void genllvm_make_floyd_runtime_init(llvmgen_t& gen_acc, const semantic_ast_t& semantic_ast, const std::vector<global_v_t>& global_symbol_table){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(semantic_ast.check_invariant());

	llvm::Function* f = make_function_stub(
		gen_acc,
		"floyd_runtime_init",
		floyd::typeid_t::make_function(floyd::typeid_t::make_int(), {}, floyd::epure::impure)
	);
	QUARK_ASSERT(check_invariant__function(f));

	llvm::BasicBlock* entryBB = llvm::BasicBlock::Create(gen_acc.instance->context, "entry", f);
	gen_acc.builder.SetInsertPoint(entryBB);

	{
		start_function_emit(gen_acc, global_symbol_table, f);
		genllvm_statements(gen_acc, semantic_ast._tree._globals._statements);

		llvm::Value* dummy_result = llvm::ConstantInt::get(gen_acc.builder.getInt64Ty(), 667);
		gen_acc.builder.CreateRet(dummy_result);
//		llvm::ReturnInst::Create(gen_acc.instance->context, dummy_result, entryBB);

		end_function_emit(gen_acc);
	}
	QUARK_TRACE_SS("result = " << floyd::print_gen(gen_acc));
	QUARK_ASSERT(check_invariant__function(f));
	QUARK_ASSERT(gen_acc.check_invariant());

	gen_acc.floyd_runtime_init_f = f;
}

void genllvm_all(llvmgen_t& gen_acc, const semantic_ast_t& semantic_ast){
	QUARK_ASSERT(gen_acc.check_invariant());
	QUARK_ASSERT(semantic_ast.check_invariant());

	//	Register all function defs as LLVM function prototypes.
	//	host functions are later linked by LLVM execution engine, by matching the function names.
	//	Floyd functions are later filled with instructions.
	{
		const auto& defs = semantic_ast._tree._function_defs;
		for(int function_id = 0 ; function_id < defs.size() ; function_id++){
			const auto& function_def = *defs[function_id];
			const auto function_type = function_def._function_type;

			if(function_def._host_function_id != k_no_host_function_id){
				const auto label = make_host_function_label(function_def._host_function_id);
				auto f = make_function_stub(gen_acc, label, hack_function_type(function_type));
			}
			else{
				const auto label = make_unique_internal_function_name2(function_id);
				auto f = make_function_stub(gen_acc, label, hack_function_type(function_type));
			}
		}
	}

	//	Global variables.
	{
		QUARK_ASSERT(gen_acc.check_invariant());
		QUARK_ASSERT(semantic_ast.check_invariant());

		std::vector<global_v_t> globals = genllvm_make_globals(
			gen_acc,
			semantic_ast,
			semantic_ast._tree._globals._symbol_table
		);
		gen_acc.globals = globals;
	}

	//	Global instructions are packed into the "floyd_runtime_init"-function. LLVM doesn't have global instructions.
	genllvm_make_floyd_runtime_init(gen_acc, semantic_ast, gen_acc.globals);

	//	Generate instructions for all functions.
	genllvm_make_function_defs(gen_acc, semantic_ast);
}

std::unique_ptr<llvm_ir_program_t> generate_llvm_ir(llvm_instance_t& instance, const semantic_ast_t& ast, const std::string& module_name){
	QUARK_ASSERT(instance.check_invariant());
	QUARK_ASSERT(ast.check_invariant());
	QUARK_ASSERT(module_name.empty() == false);
	QUARK_TRACE_SS("INPUT:  " << json_to_pretty_string(semantic_ast_to_json(ast)._value));

	llvmgen_t acc(instance, module_name);
	genllvm_all(acc, ast);

	auto result = std::make_unique<llvm_ir_program_t>(&instance, acc.module, ast._tree._globals._symbol_table);
	QUARK_TRACE_SS("result = " << floyd::print_program(*result));
	return result;
}



/*
  /// For MCJIT execution engines, clients are encouraged to use the
  /// "GetFunctionAddress" method (rather than runFunction) and cast the
  /// returned uint64_t to the desired function pointer type. However, for
  /// backwards compatibility MCJIT's implementation can execute 'main-like'
  /// function (i.e. those returning void or int, and taking either no
  /// arguments or (int, char*[])).
*/
/*
LLVM’s “eager” JIT compiler is safe to use in threaded programs. Multiple threads can call ExecutionEngine::getPointerToFunction() or ExecutionEngine::runFunction() concurrently, and multiple threads can run code output by the JIT concurrently. The user must still ensure that only one thread accesses IR in a given LLVMContext while another thread might be modifying it. One way to do that is to always hold the JIT lock while accessing IR outside the JIT (the JIT modifies the IR by adding CallbackVHs). Another way is to only call getPointerToFunction() from the LLVMContext’s thread.
*/
/*

	llvm::Function* init_func = program.module->getFunction("floyd_runtime_init");
	QUARK_ASSERT(init_func != nullptr);
	init_func->print(llvm::errs());



		llvm::GenericValue init_result = ee->runFunction(init_func, {});
		const int64_t init_result_int = init_result.IntVal.getSExtValue();
		QUARK_ASSERT(init_result_int == 667);



		llvm::Function* b_func = ee->FindFunctionNamed("floyd_runtime_init");
		llvm::GenericValue b_result = ee->runFunction(b_func, {});
		const int64_t b_result_int = b_result.IntVal.getSExtValue();
		QUARK_ASSERT(b_result_int == 667);

*/

void* get_global_ptr(llvm_execution_engine_t& ee, const std::string& name){
	QUARK_ASSERT(ee.check_invariant());
	QUARK_ASSERT(name.empty() == false);

	const auto addr = ee.ee->getGlobalValueAddress(name);
	return  (void*)addr;
}

void* get_global_function(llvm_execution_engine_t& ee, const std::string& name){
	QUARK_ASSERT(ee.check_invariant());
	QUARK_ASSERT(name.empty() == false);

	const auto addr = ee.ee->getFunctionAddress(name);
	return (void*)addr;
}




//	Destroys program, can only run it once!
llvm_execution_engine_t make_engine_no_init(llvm_instance_t& instance, llvm_ir_program_t& program_breaks){
	QUARK_ASSERT(instance.check_invariant());
	QUARK_ASSERT(program_breaks.check_invariant());

	std::string collectedErrors;

	//??? Destroys p -- uses std::move().
	llvm::ExecutionEngine* exeEng = llvm::EngineBuilder(std::move(program_breaks.module))
		.setErrorStr(&collectedErrors)
		.setOptLevel(llvm::CodeGenOpt::Level::None)
		.setVerifyModules(true)
		.setEngineKind(llvm::EngineKind::JIT)
		.create();

	if (exeEng == nullptr){
		std::string error = "Unable to construct execution engine: " + collectedErrors;
		perror(error.c_str());
		throw std::exception();
	}
	QUARK_ASSERT(collectedErrors.empty());

	auto ee = std::shared_ptr<llvm::ExecutionEngine>(exeEng);
	ee->finalizeObject();

	auto ee2 = llvm_execution_engine_t{ k_debug_magic, ee, {} };
	QUARK_ASSERT(ee2.check_invariant());


#if DEBUG
	//	Verify that all global functions can be accessed. If *one* is unresolved, then all return NULL!?
	{
		int index = 0;
		for(const auto& e: program_breaks.debug_globals._symbols){
			if(e.second.get_type().is_function()){
				const auto global_var = (FLOYD_RUNTIME_HOST_FUNCTION*)floyd::get_global_ptr(ee2, e.first);
				QUARK_ASSERT(global_var != nullptr);

				const auto f = *global_var;
				QUARK_ASSERT(f != nullptr);

				const std::string suffix = f == nullptr ? " NULL POINTER" : "";
				const uint64_t addr = reinterpret_cast<uint64_t>(f);
				QUARK_TRACE_SS(index << " " << e.first << " " << addr << suffix);
			}
			else{
			}
			index++;
		}
	}
#endif

//	ee2.ee->DisableGVCompilation(false);
//	ee2.ee->DisableSymbolSearching(false);

//	llvm::WriteBitcodeToFile(exeEng->getVerifyModules(), raw_ostream &Out);
	return ee2;
}

uint64_t call_floyd_runtime_init(llvm_execution_engine_t& ee){
	QUARK_ASSERT(ee.check_invariant());

	auto a_func = reinterpret_cast<FLOYD_RUNTIME_INIT>(get_global_function(ee, "floyd_runtime_init"));
	QUARK_ASSERT(a_func != nullptr);

	int64_t a_result = (*a_func)((void*)&ee);
	QUARK_ASSERT(a_result == 667);
	return a_result;
}

//	Destroys program, can only run it once!
//	Automatically runs floyd_runtime_init() to execute Floyd's global functions and initialize global constants.
llvm_execution_engine_t make_engine_run_init(llvm_instance_t& instance, llvm_ir_program_t& program_breaks){
	QUARK_ASSERT(instance.check_invariant());
	QUARK_ASSERT(program_breaks.check_invariant());

	llvm_execution_engine_t ee = make_engine_no_init(instance, program_breaks);

#if DEBUG
	{
		const auto print_global_ptr = (FLOYD_RUNTIME_HOST_FUNCTION*)floyd::get_global_ptr(ee, "print");
		QUARK_ASSERT(print_global_ptr != nullptr);

		const auto print_f = *print_global_ptr;
		if(print_f){
			QUARK_ASSERT(print_f != nullptr);

//			(*print_f)(&ee, 109);
		}
	}

	{
		auto a_func = reinterpret_cast<FLOYD_RUNTIME_INIT>(get_global_function(ee, "floyd_runtime_init"));
		QUARK_ASSERT(a_func != nullptr);
	}
#endif

	const auto init_result = call_floyd_runtime_init(ee);
	QUARK_ASSERT(init_result == 667);

	return ee;
}


//	Destroys program, can only run it once!
int64_t run_llvm_program(llvm_instance_t& instance, llvm_ir_program_t& program_breaks, const std::vector<floyd::value_t>& args){
	QUARK_ASSERT(instance.check_invariant());
	QUARK_ASSERT(program_breaks.check_invariant());

	auto ee = make_engine_run_init(instance, program_breaks);
	return 0;
}




/*
	auto gv = program.module->getGlobalVariable("result");
	const auto p3 = exeEng->getPointerToGlobal(gv);

	const auto result = *(uint64_t*)p3;

	const auto p = exeEng->getPointerToGlobalIfAvailable("result");
	llvm::GlobalVariable* p2 = exeEng->FindGlobalVariableNamed("result", true);
*/


////////////////////////////////		HELPERS



std::unique_ptr<llvm_ir_program_t> compile_to_ir_helper(llvm_instance_t& instance, const std::string& program, const std::string& file){
	QUARK_ASSERT(instance.check_invariant());
	const auto pass3 = compile_to_sematic_ast__errors(program, file, compilation_unit_mode::k_no_core_lib);
	auto bc = generate_llvm_ir(instance, pass3, file);
	return bc;
}


int64_t run_using_llvm_helper(const std::string& program_source, const std::string& file, const std::vector<floyd::value_t>& args){
	const auto pass3 = compile_to_sematic_ast__errors(program_source, file, compilation_unit_mode::k_no_core_lib);

	llvm_instance_t instance;
	auto program = generate_llvm_ir(instance, pass3, file);
	const auto result = run_llvm_program(instance, *program, args);
	QUARK_TRACE_SS("Fib = " << result);
	return result;
}



}	//	namespace floyd




////////////////////////////////		TESTS


#define LLVM_TEST		QUARK_UNIT_TEST
#define LLVM_TEST_VIP	QUARK_UNIT_TEST_VIP


const std::string test_1_json = R"ABCD(
{
	"function_defs": [
		[["func", "^void", ["^void"], true], [], null, 2002],
		[["func", "^void", ["^void"], true], [], null, 2003],
		[["func", "^void", ["^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }], null, 1000]
	],
	"globals": {
		"statements": [[0, "store2", 0, 2, ["+", ["+", ["k", 1, "^int"], ["k", 2, "^int"], "^int"], ["k", 3, "^int"], "^int"]]],
		"symbols": [
			[0, "assert", { "init": { "function_id": 0 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^**dyn**"], true] }],
			[1, "print", { "init": { "function_id": 2 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^**dyn**"], true] }],
			[2, "result", { "init": null, "symbol_type": "immutable_local", "value_type": "^int" }]
		]
	}
}
}
")ABCD";


LLVM_TEST("", "From JSON: Check that floyd_runtime_init() runs and sets 'result' global", "", ""){
	std::pair<json_t, seq_t> a = parse_json(seq_t(test_1_json));

	floyd::llvm_instance_t instance;
	const auto pass3 = floyd::json_to_semantic_ast(floyd::ast_json_t::make(a.first));
	auto program = generate_llvm_ir(instance, pass3, "myfile.floyd");
	auto ee = make_engine_run_init(instance, *program);

	const auto result = *static_cast<uint64_t*>(floyd::get_global_ptr(ee, "result"));
	QUARK_ASSERT(result == 6);

//	QUARK_TRACE_SS("result = " << floyd::print_program(*program));
}

LLVM_TEST("", "From source: Check that floyd_runtime_init() runs and sets 'result' global", "", ""){
//	ut_verify_global_result(QUARK_POS, "let int result = 1 + 2 + 3", value_t::make_int(6));

	const auto pass3 = compile_to_sematic_ast__errors("let int result = 1 + 2 + 3", "myfile.floyd", floyd::compilation_unit_mode::k_no_core_lib);

	floyd::llvm_instance_t instance;
	auto program = generate_llvm_ir(instance, pass3, "myfile.floyd");
	auto ee = make_engine_run_init(instance, *program);

	const auto result = *static_cast<uint64_t*>(floyd::get_global_ptr(ee, "result"));
	QUARK_ASSERT(result == 6);

//	QUARK_TRACE_SS("result = " << floyd::print_program(*program));
}






const std::string test_2_json = R"ABCD(
{
	"function_defs": [
		[["func", "^void", ["^void"], true], [], null, 2002],
		[["func", "^void", ["^void"], true], [], null, 2003],
		[["func", "^void", ["^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }], null, 1000]
	],
	"globals": {
		"statements": [[0, "expression-statement", ["call", ["@i", -1, 1, ["func", "^void", ["^**dyn**"], true]], [["k", 6, "^int"]], "^void"]]],
		"symbols": [
			[0, "assert", { "init": { "function_id": 0 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^**dyn**"], true] }],
			[1, "print", { "init": { "function_id": 2 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^**dyn**"], true] }]
		]
	}
}
}
")ABCD";

#if 1
//	Works! Calls print()!!!
LLVM_TEST("", "From JSON: Simple function call, call print() from floyd_runtime_init()", "", ""){
	std::pair<json_t, seq_t> a = parse_json(seq_t(test_2_json));
	const auto pass3 = floyd::json_to_semantic_ast(floyd::ast_json_t::make(a.first));

	floyd::llvm_instance_t instance;
	auto program = generate_llvm_ir(instance, pass3, "myfile.floyd");
	auto ee = make_engine_run_init(instance, *program);
	QUARK_ASSERT(ee._print_output == std::vector<std::string>{"6"});
}
#endif

#if 1
//??? all external functions referenced from code must be defined or print() will return nullptr.
//	BROKEN!
LLVM_TEST("", "From JSON: Simple function call, call print() from floyd_runtime_init()", "", ""){
	const auto pass3 = compile_to_sematic_ast__errors("print(5)", "myfile.floyd", floyd::compilation_unit_mode::k_no_core_lib);

	floyd::llvm_instance_t instance;
	auto program = generate_llvm_ir(instance, pass3, "myfile.floyd");
	auto ee = make_engine_run_init(instance, *program);
	QUARK_ASSERT(ee._print_output == std::vector<std::string>{"5"});
}
#endif

const std::string test_3_json = R"ABCD(

{
	"function_defs": [
		[["func", "^void", ["^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }], null, 1001],
		[
			["func", ["^struct", [[{ "name": "ascii40", "type": "^string" }]]], [["^struct", [[{ "name": "bytes", "type": "^string" }]]]], true],
			[{ "name": "dummy", "type": ["^struct", [[{ "name": "bytes", "type": "^string" }]]] }],
			null,
			1032
		],
		[["func", ["^struct", [[{ "name": "ascii40", "type": "^string" }]]], ["^string"], true], [{ "name": "dummy", "type": "^string" }], null, 1031],
		[["func", "^void", ["^string"], false], [{ "name": "dummy", "type": "^string" }], null, 1028],
		[["func", "^void", ["^string"], false], [{ "name": "dummy", "type": "^string" }], null, 1029],
		[["func", "^bool", ["^string"], false], [{ "name": "dummy", "type": "^string" }], null, 1027],
		[["func", "^**dyn**", ["^**dyn**", "^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }], null, 1010],
		[["func", "^bool", ["^**dyn**", "^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }], null, 1009],
		[["func", "^**dyn**", ["^**dyn**", "^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }], null, 1036],
		[["func", "^int", ["^**dyn**", "^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }], null, 1008],
		[
			[
				"func",
				[
					"^struct",
					[
						[
							{ "name": "home_dir", "type": "^string" },
							{ "name": "documents_dir", "type": "^string" },
							{ "name": "desktop_dir", "type": "^string" },
							{ "name": "hidden_persistence_dir", "type": "^string" },
							{ "name": "preferences_dir", "type": "^string" },
							{ "name": "cache_dir", "type": "^string" },
							{ "name": "temp_dir", "type": "^string" },
							{ "name": "executable_dir", "type": "^string" }
						]
					]
				],
				[],
				false
			],
			[],
			null,
			1026
		],
		[
			[
				"func",
				[
					"vector",
					["^struct", [[{ "name": "type", "type": "^string" }, { "name": "name", "type": "^string" }, { "name": "parent_path", "type": "^string" }]]]
				],
				["^string"],
				false
			],
			[{ "name": "dummy", "type": "^string" }],
			null,
			1024
		],
		[
			[
				"func",
				[
					"vector",
					["^struct", [[{ "name": "type", "type": "^string" }, { "name": "name", "type": "^string" }, { "name": "parent_path", "type": "^string" }]]]
				],
				["^string"],
				false
			],
			[{ "name": "dummy", "type": "^string" }],
			null,
			1023
		],
		[
			[
				"func",
				[
					"^struct",
					[
						[
							{ "name": "type", "type": "^string" },
							{ "name": "name", "type": "^string" },
							{ "name": "parent_path", "type": "^string" },
							{ "name": "creation_date", "type": "^string" },
							{ "name": "modification_date", "type": "^string" },
							{ "name": "file_size", "type": "^int" }
						]
					]
				],
				["^string"],
				false
			],
			[{ "name": "dummy", "type": "^string" }],
			null,
			1025
		],
		[["func", "^int", ["^json_value"], true], [{ "name": "dummy", "type": "^json_value" }], null, 1021],
		[["func", "^int", [], false], [], null, 1005],
		[["func", "^string", ["^json_value"], true], [{ "name": "dummy", "type": "^json_value" }], null, 1018],
		[
			["func", "^**dyn**", ["^json_value", "^typeid"], true],
			[{ "name": "dummy", "type": "^json_value" }, { "name": "dummy", "type": "^typeid" }],
			null,
			1020
		],
		[["func", "^**dyn**", ["^**dyn**", "^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }], null, 1033],
		[
			["func", "^string", ["^string", ["func", "^string", ["^string"], true]], true],
			[{ "name": "dummy", "type": "^string" }, { "name": "dummy", "type": ["func", "^string", ["^string"], true] }],
			null,
			1034
		],
		[["func", "^void", ["^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }], null, 1000],
		[["func", "^**dyn**", ["^**dyn**", "^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }], null, 1011],
		[["func", "^string", ["^string"], false], [{ "name": "dummy", "type": "^string" }], null, 1015],
		[
			["func", "^**dyn**", ["^**dyn**", "^**dyn**", "^**dyn**"], true],
			[{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }],
			null,
			1035
		],
		[["func", "^void", ["^string", "^string"], false], [{ "name": "dummy", "type": "^string" }, { "name": "dummy", "type": "^string" }], null, 1030],
		[
			["func", "^**dyn**", ["^**dyn**", "^int", "^int", "^**dyn**"], true],
			[
				{ "name": "dummy", "type": "^**dyn**" },
				{ "name": "dummy", "type": "^int" },
				{ "name": "dummy", "type": "^int" },
				{ "name": "dummy", "type": "^**dyn**" }
			],
			null,
			1013
		],
		[["func", "^json_value", ["^string"], true], [{ "name": "dummy", "type": "^string" }], null, 1017],
		[["func", "^void", ["^string", "^json_value"], false], [{ "name": "dummy", "type": "^string" }, { "name": "dummy", "type": "^json_value" }], null, 1022],
		[["func", "^int", ["^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }], null, 1007],
		[
			["func", "^**dyn**", ["^**dyn**", "^int", "^int"], true],
			[{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^int" }, { "name": "dummy", "type": "^int" }],
			null,
			1012
		],
		[
			["func", "^**dyn**", ["^**dyn**", "^**dyn**", "^**dyn**"], true],
			[{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }],
			null,
			1037
		],
		[["func", "^string", ["^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }], null, 1003],
		[["func", "^string", ["^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }], null, 1002],
		[["func", "^typeid", ["^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }], null, 1004],
		[
			["func", "^**dyn**", ["^**dyn**", "^**dyn**", "^**dyn**"], true],
			[{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }],
			null,
			1006
		],
		[["func", "^json_value", ["^**dyn**"], true], [{ "name": "dummy", "type": "^**dyn**" }], null, 1019],
		[["func", "^void", ["^**dyn**", "^**dyn**"], false], [{ "name": "dummy", "type": "^**dyn**" }, { "name": "dummy", "type": "^**dyn**" }], null, 1016]
	],
	"globals": {
		"statements": [[0, "expression-statement", ["call", ["@i", -1, 20, ["func", "^void", ["^**dyn**"], true]], [["k", 5, "^int"]], "^void"]]],
		"symbols": [
			[0, "assert", { "init": { "function_id": 0 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^**dyn**"], true] }],
			[
				1,
				"calc_binary_sha1",
				{
					"init": { "function_id": 1 },
					"symbol_type": "immutable_local",
					"value_type": [
						"func",
						["^struct", [[{ "name": "ascii40", "type": "^string" }]]],
						[["^struct", [[{ "name": "bytes", "type": "^string" }]]]],
						true
					]
				}
			],
			[
				2,
				"calc_string_sha1",
				{
					"init": { "function_id": 2 },
					"symbol_type": "immutable_local",
					"value_type": ["func", ["^struct", [[{ "name": "ascii40", "type": "^string" }]]], ["^string"], true]
				}
			],
			[
				3,
				"create_directory_branch",
				{ "init": { "function_id": 3 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^string"], false] }
			],
			[4, "delete_fsentry_deep", { "init": { "function_id": 4 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^string"], false] }],
			[5, "does_fsentry_exist", { "init": { "function_id": 5 }, "symbol_type": "immutable_local", "value_type": ["func", "^bool", ["^string"], false] }],
			[6, "erase", { "init": { "function_id": 6 }, "symbol_type": "immutable_local", "value_type": ["func", "^**dyn**", ["^**dyn**", "^**dyn**"], true] }],
			[7, "exists", { "init": { "function_id": 7 }, "symbol_type": "immutable_local", "value_type": ["func", "^bool", ["^**dyn**", "^**dyn**"], true] }],
			[
				8,
				"filter",
				{ "init": { "function_id": 8 }, "symbol_type": "immutable_local", "value_type": ["func", "^**dyn**", ["^**dyn**", "^**dyn**"], true] }
			],
			[9, "find", { "init": { "function_id": 9 }, "symbol_type": "immutable_local", "value_type": ["func", "^int", ["^**dyn**", "^**dyn**"], true] }],
			[
				10,
				"get_fs_environment",
				{
					"init": { "function_id": 10 },
					"symbol_type": "immutable_local",
					"value_type": [
						"func",
						[
							"^struct",
							[
								[
									{ "name": "home_dir", "type": "^string" },
									{ "name": "documents_dir", "type": "^string" },
									{ "name": "desktop_dir", "type": "^string" },
									{ "name": "hidden_persistence_dir", "type": "^string" },
									{ "name": "preferences_dir", "type": "^string" },
									{ "name": "cache_dir", "type": "^string" },
									{ "name": "temp_dir", "type": "^string" },
									{ "name": "executable_dir", "type": "^string" }
								]
							]
						],
						[],
						false
					]
				}
			],
			[
				11,
				"get_fsentries_deep",
				{
					"init": { "function_id": 11 },
					"symbol_type": "immutable_local",
					"value_type": [
						"func",
						[
							"vector",
							[
								"^struct",
								[[{ "name": "type", "type": "^string" }, { "name": "name", "type": "^string" }, { "name": "parent_path", "type": "^string" }]]
							]
						],
						["^string"],
						false
					]
				}
			],
			[
				12,
				"get_fsentries_shallow",
				{
					"init": { "function_id": 12 },
					"symbol_type": "immutable_local",
					"value_type": [
						"func",
						[
							"vector",
							[
								"^struct",
								[[{ "name": "type", "type": "^string" }, { "name": "name", "type": "^string" }, { "name": "parent_path", "type": "^string" }]]
							]
						],
						["^string"],
						false
					]
				}
			],
			[
				13,
				"get_fsentry_info",
				{
					"init": { "function_id": 13 },
					"symbol_type": "immutable_local",
					"value_type": [
						"func",
						[
							"^struct",
							[
								[
									{ "name": "type", "type": "^string" },
									{ "name": "name", "type": "^string" },
									{ "name": "parent_path", "type": "^string" },
									{ "name": "creation_date", "type": "^string" },
									{ "name": "modification_date", "type": "^string" },
									{ "name": "file_size", "type": "^int" }
								]
							]
						],
						["^string"],
						false
					]
				}
			],
			[14, "get_json_type", { "init": { "function_id": 14 }, "symbol_type": "immutable_local", "value_type": ["func", "^int", ["^json_value"], true] }],
			[15, "get_time_of_day", { "init": { "function_id": 15 }, "symbol_type": "immutable_local", "value_type": ["func", "^int", [], false] }],
			[
				16,
				"jsonvalue_to_script",
				{ "init": { "function_id": 16 }, "symbol_type": "immutable_local", "value_type": ["func", "^string", ["^json_value"], true] }
			],
			[
				17,
				"jsonvalue_to_value",
				{ "init": { "function_id": 17 }, "symbol_type": "immutable_local", "value_type": ["func", "^**dyn**", ["^json_value", "^typeid"], true] }
			],
			[18, "map", { "init": { "function_id": 18 }, "symbol_type": "immutable_local", "value_type": ["func", "^**dyn**", ["^**dyn**", "^**dyn**"], true] }],
			[
				19,
				"map_string",
				{
					"init": { "function_id": 19 },
					"symbol_type": "immutable_local",
					"value_type": ["func", "^string", ["^string", ["func", "^string", ["^string"], true]], true]
				}
			],
			[20, "print", { "init": { "function_id": 20 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^**dyn**"], true] }],
			[
				21,
				"push_back",
				{ "init": { "function_id": 21 }, "symbol_type": "immutable_local", "value_type": ["func", "^**dyn**", ["^**dyn**", "^**dyn**"], true] }
			],
			[22, "read_text_file", { "init": { "function_id": 22 }, "symbol_type": "immutable_local", "value_type": ["func", "^string", ["^string"], false] }],
			[
				23,
				"reduce",
				{
					"init": { "function_id": 23 },
					"symbol_type": "immutable_local",
					"value_type": ["func", "^**dyn**", ["^**dyn**", "^**dyn**", "^**dyn**"], true]
				}
			],
			[
				24,
				"rename_fsentry",
				{ "init": { "function_id": 24 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^string", "^string"], false] }
			],
			[
				25,
				"replace",
				{
					"init": { "function_id": 25 },
					"symbol_type": "immutable_local",
					"value_type": ["func", "^**dyn**", ["^**dyn**", "^int", "^int", "^**dyn**"], true]
				}
			],
			[
				26,
				"script_to_jsonvalue",
				{ "init": { "function_id": 26 }, "symbol_type": "immutable_local", "value_type": ["func", "^json_value", ["^string"], true] }
			],
			[
				27,
				"send",
				{ "init": { "function_id": 27 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^string", "^json_value"], false] }
			],
			[28, "size", { "init": { "function_id": 28 }, "symbol_type": "immutable_local", "value_type": ["func", "^int", ["^**dyn**"], true] }],
			[
				29,
				"subset",
				{ "init": { "function_id": 29 }, "symbol_type": "immutable_local", "value_type": ["func", "^**dyn**", ["^**dyn**", "^int", "^int"], true] }
			],
			[
				30,
				"supermap",
				{
					"init": { "function_id": 30 },
					"symbol_type": "immutable_local",
					"value_type": ["func", "^**dyn**", ["^**dyn**", "^**dyn**", "^**dyn**"], true]
				}
			],
			[31, "to_pretty_string", { "init": { "function_id": 31 }, "symbol_type": "immutable_local", "value_type": ["func", "^string", ["^**dyn**"], true] }],
			[32, "to_string", { "init": { "function_id": 32 }, "symbol_type": "immutable_local", "value_type": ["func", "^string", ["^**dyn**"], true] }],
			[33, "typeof", { "init": { "function_id": 33 }, "symbol_type": "immutable_local", "value_type": ["func", "^typeid", ["^**dyn**"], true] }],
			[
				34,
				"update",
				{
					"init": { "function_id": 34 },
					"symbol_type": "immutable_local",
					"value_type": ["func", "^**dyn**", ["^**dyn**", "^**dyn**", "^**dyn**"], true]
				}
			],
			[
				35,
				"value_to_jsonvalue",
				{ "init": { "function_id": 35 }, "symbol_type": "immutable_local", "value_type": ["func", "^json_value", ["^**dyn**"], true] }
			],
			[
				36,
				"write_text_file",
				{ "init": { "function_id": 36 }, "symbol_type": "immutable_local", "value_type": ["func", "^void", ["^**dyn**", "^**dyn**"], false] }
			],
			[37, "null", { "init": null, "symbol_type": "immutable_local", "value_type": "^json_value" }],
			[38, "**undef**", { "init": null, "symbol_type": "immutable_local", "value_type": "^**undef**" }],
			[39, "**dyn**", { "init": null, "symbol_type": "immutable_local", "value_type": "^**dyn**" }],
			[40, "void", { "init": null, "symbol_type": "immutable_local", "value_type": "^void" }],
			[41, "bool", { "init": "^bool", "symbol_type": "immutable_local", "value_type": "^typeid" }],
			[42, "int", { "init": "^int", "symbol_type": "immutable_local", "value_type": "^typeid" }],
			[43, "double", { "init": "^double", "symbol_type": "immutable_local", "value_type": "^typeid" }],
			[44, "string", { "init": "^string", "symbol_type": "immutable_local", "value_type": "^typeid" }],
			[45, "typeid", { "init": "^typeid", "symbol_type": "immutable_local", "value_type": "^typeid" }],
			[46, "json_value", { "init": "^json_value", "symbol_type": "immutable_local", "value_type": "^typeid" }],
			[47, "json_object", { "init": 1, "symbol_type": "immutable_local", "value_type": "^int" }],
			[48, "json_array", { "init": 2, "symbol_type": "immutable_local", "value_type": "^int" }],
			[49, "json_string", { "init": 3, "symbol_type": "immutable_local", "value_type": "^int" }],
			[50, "json_number", { "init": 4, "symbol_type": "immutable_local", "value_type": "^int" }],
			[51, "json_true", { "init": 5, "symbol_type": "immutable_local", "value_type": "^int" }],
			[52, "json_false", { "init": 6, "symbol_type": "immutable_local", "value_type": "^int" }],
			[53, "json_null", { "init": 7, "symbol_type": "immutable_local", "value_type": "^int" }]
		]
	}
}
")ABCD";


LLVM_TEST("", "json_to_semantic_ast()", "Complex JSON with ^types", ""){
	std::pair<json_t, seq_t> a = parse_json(seq_t(test_3_json));
	const auto pass3 = floyd::json_to_semantic_ast(floyd::ast_json_t::make(a.first));
}

LLVM_TEST("", "From JSON: Simple function call, call print() from floyd_runtime_init()", "", ""){
	std::pair<json_t, seq_t> a = parse_json(seq_t(test_3_json));
	const auto pass3 = floyd::json_to_semantic_ast(floyd::ast_json_t::make(a.first));

	floyd::llvm_instance_t instance;
	auto program = generate_llvm_ir(instance, pass3, "myfile.floyd");
	auto ee = make_engine_run_init(instance, *program);
	QUARK_ASSERT(ee._print_output == std::vector<std::string>{"5"});
}

