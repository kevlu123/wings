#include "compile.h"
#include "common.h"

#include <unordered_map>
#include <stack>

namespace wings {

	static thread_local std::stack<std::vector<size_t>> breakInstructions;
	static thread_local std::stack<std::vector<size_t>> continueInstructions;
	static thread_local std::stack<std::optional<size_t>> forLoopBreakInstructions;

	static void CompileBody(const std::vector<Statement>& body, std::vector<Instruction>& instructions);
	static void CompileExpression(const Expression& expression, std::vector<Instruction>& instructions);
	static void CompileFunction(const Expression& node, std::vector<Instruction>& instructions);

	static const std::unordered_map<Operation, std::string> OP_METHODS = {
		{ Operation::Index,  "__getitem__"  },
		{ Operation::Pos,	 "__pos__"      },
		{ Operation::Neg,	 "__neg__"      },
		{ Operation::Add,	 "__add__"      },
		{ Operation::Sub,	 "__sub__"      },
		{ Operation::Mul,	 "__mul__"      },
		{ Operation::Div,	 "__truediv__"  },
		{ Operation::IDiv,	 "__floordiv__" },
		{ Operation::Mod,	 "__mod__"      },
		{ Operation::Pow,	 "__pow__"      },
		{ Operation::Eq,	 "__eq__"       },
		{ Operation::Ne,	 "__ne__"       },
		{ Operation::Lt,	 "__lt__"       },
		{ Operation::Le,	 "__le__"       },
		{ Operation::Gt,	 "__gt__"       },
		{ Operation::Ge,	 "__ge__"       },
		{ Operation::In,	 "__contains__" },
		{ Operation::BitAnd, "__and__"      },
		{ Operation::BitOr,  "__or__"       },
		{ Operation::BitNot, "__invert__"   },
		{ Operation::BitXor, "__xor__"      },
		{ Operation::ShiftL, "__lshift__"   },
		{ Operation::ShiftR, "__rshift__"   },

		{ Operation::AddAssign, "__iadd__"       },
		{ Operation::SubAssign, "__isub__"       },
		{ Operation::MulAssign, "__imul__"       },
		{ Operation::DivAssign, "__itruediv__"   },
		{ Operation::IDivAssign, "__ifloordiv__" },
		{ Operation::ModAssign, "__imod__"       },
		{ Operation::PowAssign, "__ipow__"       },
		{ Operation::AndAssign, "__iand__"      },
		{ Operation::OrAssign, "__ior__"         },
		{ Operation::XorAssign, "__ixor__"       },
		{ Operation::ShiftLAssign, "__ilshift__" },
		{ Operation::ShiftRAssign, "__irshift__" },
	};

	static const std::unordered_set<Operation> COMPOUND_OPS = {
		Operation::AddAssign,
		Operation::SubAssign,
		Operation::MulAssign,
		Operation::PowAssign,
		Operation::DivAssign,
		Operation::IDivAssign,
		Operation::ModAssign,
		Operation::ShiftLAssign,
		Operation::ShiftRAssign,
		Operation::OrAssign,
		Operation::AndAssign,
		Operation::XorAssign,
	};

	static void CompileInlineIfElse(const Expression& expression, std::vector<Instruction>& instructions) {
		const auto& condition = expression.children[0];
		const auto& trueCase = expression.children[1];
		const auto& falseCase = expression.children[2];
		
		CompileExpression(condition, instructions);

		Instruction falseJump{};
		falseJump.srcPos = condition.srcPos;
		falseJump.type = Instruction::Type::JumpIfFalsePop;
		falseJump.jump = std::make_unique<JumpInstruction>();
		size_t falseJumpIndex = instructions.size();
		instructions.push_back(std::move(falseJump));

		CompileExpression(trueCase, instructions);

		Instruction trueJump{};
		trueJump.srcPos = condition.srcPos;
		trueJump.type = Instruction::Type::Jump;
		trueJump.jump = std::make_unique<JumpInstruction>();
		size_t trueJumpIndex = instructions.size();
		instructions.push_back(std::move(trueJump));

		instructions[falseJumpIndex].jump->location = instructions.size();

		CompileExpression(falseCase, instructions);

		instructions[trueJumpIndex].jump->location = instructions.size();
	}

	static void CompileShortcircuitLogical(const Expression& expr, std::vector<Instruction>& instructions) {
		const auto& lhs = expr.children[0];
		const auto& rhs = expr.children[1];

		CompileExpression(lhs, instructions);
		
		size_t jmpInstrIndex = instructions.size();
		Instruction jmp{};
		if (expr.operation == Operation::And) {
			jmp.type = Instruction::Type::JumpIfFalse;
		} else {
			jmp.type = Instruction::Type::JumpIfTrue;
		}
		jmp.srcPos = expr.srcPos;
		jmp.jump = std::make_unique<JumpInstruction>();
		instructions.push_back(std::move(jmp));

		CompileExpression(rhs, instructions);
		
		instructions[jmpInstrIndex].jump->location = instructions.size();
	}

	static void CompileIn(const Expression& expression, std::vector<Instruction>& instructions) {
		Instruction argFrame{};
		argFrame.srcPos = expression.srcPos;
		argFrame.type = Instruction::Type::PushArgFrame;
		instructions.push_back(std::move(argFrame));

		CompileExpression(expression.children[1], instructions);

		Instruction dot{};
		dot.srcPos = expression.srcPos;
		dot.type = Instruction::Type::Dot;
		dot.string = std::make_unique<StringArgInstruction>();
		dot.string->string = "__contains__";
		instructions.push_back(std::move(dot));

		CompileExpression(expression.children[0], instructions);

		Instruction call{};
		call.srcPos = expression.srcPos;
		call.type = Instruction::Type::Call;
		instructions.push_back(std::move(call));

		if (expression.operation == Operation::NotIn) {
			Instruction notInstr{};
			notInstr.srcPos = expression.srcPos;
			notInstr.type = Instruction::Type::Not;
			instructions.push_back(std::move(notInstr));
		}
	}

	static void CompileAssignment(
		const AssignTarget& assignTarget,
		const Expression& assignee,
		const Expression& value,
		const SourcePosition& srcPos,
		std::vector<Instruction>& instructions) {

		Instruction instr{};
		instr.srcPos = srcPos;

		switch (assignTarget.type) {
		case AssignType::Direct:
		case AssignType::Pack:
			// <assign>
			//		<assignee>
			//		<expr>
			CompileExpression(value, instructions);
			instr.directAssign = std::make_unique<DirectAssignInstruction>();
			instr.directAssign->assignTarget = assignTarget;
			instr.type = Instruction::Type::DirectAssign;
			break;
		case AssignType::Index: {
			// <assign>
			//		<assignee>
			//			<var>
			//			<index>
			//		<expr>
			Instruction argFrame{};
			argFrame.srcPos = srcPos;
			argFrame.type = Instruction::Type::PushArgFrame;
			instructions.push_back(std::move(argFrame));

			CompileExpression(assignee.children[0], instructions);

			Instruction dot{};
			dot.srcPos = srcPos;
			dot.type = Instruction::Type::Dot;
			dot.string = std::make_unique<StringArgInstruction>();
			dot.string->string = "__setitem__";
			instructions.push_back(std::move(dot));

			CompileExpression(assignee.children[1], instructions);
			CompileExpression(value, instructions);

			instr.type = Instruction::Type::Call;
			break;
		}
		case AssignType::Member:
			// <assign>
			//		<assignee>
			//			<var>
			//		<expr>
			CompileExpression(assignee.children[0], instructions);
			CompileExpression(value, instructions);
			instr.string = std::make_unique<StringArgInstruction>();
			instr.string->string = assignee.variableName;
			instr.type = Instruction::Type::MemberAssign;
			break;
		default:
			WG_UNREACHABLE();
		}

		instructions.push_back(std::move(instr));
	}

	static void CompileExpression(const Expression& expression, std::vector<Instruction>& instructions) {
		if (expression.operation == Operation::Assign) {
			CompileAssignment(expression.assignTarget, expression.children[0], expression.children[1], expression.srcPos, instructions);
			return;
		}

		auto compileChildExpressions = [&] {
			for (size_t i = 0; i < expression.children.size(); i++)
				CompileExpression(expression.children[i], instructions);
		};

		Instruction instr{};
		instr.srcPos = expression.srcPos;

		switch (expression.operation) {
		case Operation::Literal:
			instr.literal = std::make_unique<LiteralInstruction>();
			switch (expression.literalValue.type) {
			case LiteralValue::Type::Null: *instr.literal = nullptr; break;
			case LiteralValue::Type::Bool: *instr.literal = expression.literalValue.b; break;
			case LiteralValue::Type::Int: *instr.literal = expression.literalValue.i; break;
			case LiteralValue::Type::Float: *instr.literal = expression.literalValue.f; break;
			case LiteralValue::Type::String: *instr.literal = expression.literalValue.s; break;
			default: WG_UNREACHABLE();
			}
			instr.type = Instruction::Type::Literal;
			break;
		case Operation::Tuple:
		case Operation::List:
		case Operation::Map:
		case Operation::Set: {
			Instruction argFrame{};
			argFrame.srcPos = expression.srcPos;
			argFrame.type = Instruction::Type::PushArgFrame;
			instructions.push_back(std::move(argFrame));

			compileChildExpressions();

			switch (expression.operation) {
			case Operation::Tuple: instr.type = Instruction::Type::Tuple; break;
			case Operation::List: instr.type = Instruction::Type::List; break;
			case Operation::Map: instr.type = Instruction::Type::Map; break;
			case Operation::Set: instr.type = Instruction::Type::Set; break;
			default: WG_UNREACHABLE();
			}
			break;
		}
		case Operation::Variable:
			instr.string = std::make_unique<StringArgInstruction>();
			instr.string->string = expression.variableName;
			instr.type = Instruction::Type::Variable;
			break;
		case Operation::Dot:
			compileChildExpressions();
			instr.string = std::make_unique<StringArgInstruction>();
			instr.string->string = expression.variableName;
			instr.type = Instruction::Type::Dot;
			break;
		case Operation::Call: {
			Instruction pushArgFrame{};
			pushArgFrame.srcPos = expression.srcPos;
			pushArgFrame.type = Instruction::Type::PushArgFrame;
			instructions.push_back(std::move(pushArgFrame));

			compileChildExpressions();
			instr.type = Instruction::Type::Call;
			break;
		}
		case Operation::Or:
		case Operation::And:
			CompileShortcircuitLogical(expression, instructions);
			return;
		case Operation::Not:
			CompileExpression(expression.children[0], instructions);
			instr.type = Instruction::Type::Not;
			break;
		case Operation::In:
		case Operation::NotIn:
			CompileIn(expression, instructions);
			return;
		case Operation::Is:
		case Operation::IsNot:
			compileChildExpressions();
			
			instr.type = Instruction::Type::Is;
			instructions.push_back(std::move(instr));
			
			if (expression.operation == Operation::IsNot) {
				Instruction notInstr{};
				notInstr.srcPos = expression.srcPos;
				notInstr.type = Instruction::Type::Not;
				instructions.push_back(std::move(notInstr));
			}
			return;
		case Operation::IfElse:
			CompileInlineIfElse(expression, instructions);
			return;
		case Operation::Unpack:
			compileChildExpressions();
			instr.type = Instruction::Type::Unpack;
			break;
		case Operation::UnpackMapForMapCreation:
			compileChildExpressions();
			instr.type = Instruction::Type::UnpackMapForMapCreation;
			break;
		case Operation::UnpackMapForCall:
			compileChildExpressions();
			instr.type = Instruction::Type::UnpackMapForCall;
			break;
		case Operation::Slice: {
			// var.__getitem__(slice(...))
			Instruction argFrame{};
			argFrame.srcPos = expression.srcPos;
			argFrame.type = Instruction::Type::PushArgFrame;
			instructions.push_back(std::move(argFrame));

			CompileExpression(expression.children[0], instructions);

			Instruction dot{};
			dot.srcPos = expression.srcPos;
			dot.type = Instruction::Type::Dot;
			dot.string = std::make_unique<StringArgInstruction>();
			dot.string->string = "__getitem__";
			instructions.push_back(std::move(dot));

			for (size_t i = 1; i < expression.children.size(); i++)
				CompileExpression(expression.children[i], instructions);
			
			Instruction slice{};
			slice.srcPos = expression.srcPos;
			slice.type = Instruction::Type::Slice;
			instructions.push_back(std::move(slice));

			instr.type = Instruction::Type::Call;
			break;
		}
		case Operation::ListComprehension: {
			Instruction argFrame{};
			argFrame.srcPos = expression.srcPos;
			argFrame.type = Instruction::Type::PushArgFrame;
			instructions.push_back(std::move(argFrame));
			
			Instruction list{};
			list.srcPos = expression.srcPos;
			list.type = Instruction::Type::List;
			instructions.push_back(std::move(list));
			
			Instruction assign{};
			assign.srcPos = expression.srcPos;
			assign.type = Instruction::Type::DirectAssign;
			assign.directAssign = std::make_unique<DirectAssignInstruction>();
			assign.directAssign->assignTarget.type = AssignType::Direct;
			assign.directAssign->assignTarget.direct = expression.listComp.listName;
			instructions.push_back(std::move(assign));
			
			CompileBody(expression.listComp.forBody, instructions);
			return;
		}
		case Operation::Function:
			CompileFunction(expression, instructions);
			return;
		case Operation::Kwarg: {
			Instruction load{};
			load.srcPos = expression.srcPos;
			load.type = Instruction::Type::Literal;
			load.literal = std::make_unique<LiteralInstruction>();
			*load.literal = expression.variableName;
			instructions.push_back(std::move(load));

			Instruction push{};
			push.srcPos = expression.srcPos;
			push.type = Instruction::Type::PushKwarg;
			instructions.push_back(std::move(push));

			compileChildExpressions();
			return;
		}
		case Operation::CompoundAssignment:
			CompileAssignment(expression.assignTarget, expression.children[0].children[0], expression.children[0], expression.srcPos, instructions);
			return;
		default: {
			Instruction argFrame{};
			argFrame.srcPos = expression.srcPos;
			argFrame.type = Instruction::Type::PushArgFrame;
			instructions.push_back(std::move(argFrame));

			CompileExpression(expression.children[0], instructions);

			Instruction dot{};
			dot.srcPos = expression.srcPos;
			dot.type = Instruction::Type::Dot;
			dot.string = std::make_unique<StringArgInstruction>();
			dot.string->string = OP_METHODS.at(expression.operation);
			instructions.push_back(std::move(dot));

			for (size_t i = 1; i < expression.children.size(); i++)
				CompileExpression(expression.children[i], instructions);

			instr.type = Instruction::Type::Call;
		}
		}

		instructions.push_back(std::move(instr));
	}

	static void CompileExpressionStatement(const Statement& node, std::vector<Instruction>& instructions) {
		auto& expr = node.Get<stat::Expr>();
		
		CompileExpression(expr.expr, instructions);

		Instruction instr{};
		instr.srcPos = expr.expr.srcPos;
		instr.type = Instruction::Type::Pop;
		instructions.push_back(std::move(instr));
	}

	static void CompileIf(const Statement& node, std::vector<Instruction>& instructions) {
		auto& ifStat = node.Get<stat::If>();
		
		CompileExpression(ifStat.expr, instructions);

		size_t falseJumpInstrIndex = instructions.size();
		Instruction falseJump{};
		falseJump.srcPos = node.srcPos;
		falseJump.type = Instruction::Type::JumpIfFalsePop;
		falseJump.jump = std::make_unique<JumpInstruction>();
		instructions.push_back(std::move(falseJump));

		CompileBody(ifStat.body, instructions);

		if (ifStat.elseClause) {
			size_t trueJumpInstrIndex = instructions.size();
			Instruction trueJump{};
			trueJump.srcPos = ifStat.elseClause->srcPos;
			trueJump.type = Instruction::Type::Jump;
			trueJump.jump = std::make_unique<JumpInstruction>();
			instructions.push_back(std::move(trueJump));

			instructions[falseJumpInstrIndex].jump->location = instructions.size();

			CompileBody(ifStat.elseClause->Get<stat::Else>().body, instructions);

			instructions[trueJumpInstrIndex].jump->location = instructions.size();
		} else {
			instructions[falseJumpInstrIndex].jump->location = instructions.size();
		}
	}

	static void CompileWhile(const Statement& node, std::vector<Instruction>& instructions) {
		auto& whileStat = node.Get<stat::While>();
		
		size_t conditionLocation = instructions.size();
		CompileExpression(whileStat.expr, instructions);
		
		size_t terminateJumpInstrIndex = instructions.size();
		Instruction terminateJump{};
		terminateJump.srcPos = node.srcPos;
		terminateJump.type = Instruction::Type::JumpIfFalsePop;
		terminateJump.jump = std::make_unique<JumpInstruction>();
		instructions.push_back(std::move(terminateJump));

		breakInstructions.emplace();
		continueInstructions.emplace();
		forLoopBreakInstructions.emplace();
		
		CompileBody(whileStat.body, instructions);

		Instruction loopJump{};
		loopJump.srcPos = node.srcPos;
		loopJump.type = Instruction::Type::Jump;
		loopJump.jump = std::make_unique<JumpInstruction>();
		loopJump.jump->location = conditionLocation;
		instructions.push_back(std::move(loopJump));

		instructions[terminateJumpInstrIndex].jump->location = instructions.size();

		if (forLoopBreakInstructions.top()) {
			instructions[forLoopBreakInstructions.top().value()].queuedJump->location = instructions.size();
		}

		if (whileStat.elseClause) {
			CompileBody(whileStat.elseClause->Get<stat::Else>().body, instructions);
		}

		for (size_t index : breakInstructions.top()) {
			instructions[index].queuedJump->location = instructions.size();
		}
		for (size_t index : continueInstructions.top()) {
			instructions[index].queuedJump->location = conditionLocation;
		}
		
		breakInstructions.pop();
		continueInstructions.pop();
		forLoopBreakInstructions.pop();
	}

	static void CompileBreak(const Statement& node, std::vector<Instruction>& instructions) {
		auto& brk = node.Get<stat::Break>();

		if (brk.exitForLoopNormally) {
			forLoopBreakInstructions.top() = instructions.size();
		} else {
			breakInstructions.top().push_back(instructions.size());
		}

		Instruction jump{};
		jump.srcPos = node.srcPos;
		jump.type = Instruction::Type::QueueJump;
		jump.queuedJump = std::make_unique<QueuedJumpInstruction>();
		jump.queuedJump->finallyCount = brk.finallyCount;
		instructions.push_back(std::move(jump));
	}

	static void CompileContinue(const Statement& node, std::vector<Instruction>& instructions) {
		continueInstructions.top().push_back(instructions.size());

		Instruction jump{};
		jump.srcPos = node.srcPos;
		jump.type = Instruction::Type::QueueJump;
		jump.queuedJump = std::make_unique<QueuedJumpInstruction>();
		jump.queuedJump->finallyCount = node.Get<stat::Continue>().finallyCount;
		instructions.push_back(std::move(jump));
	}

	static void CompileReturn(const Statement& node, std::vector<Instruction>& instructions) {
		auto& ret = node.Get<stat::Return>();
		CompileExpression(ret.expr, instructions);

		Instruction in{};
		in.srcPos = node.srcPos;
		in.type = Instruction::Type::Return;
		in.queuedJump = std::make_unique<QueuedJumpInstruction>();
		in.queuedJump->finallyCount = ret.finallyCount;
		instructions.push_back(std::move(in));
	}

	static void CompileFunction(const Expression& node, std::vector<Instruction>& instructions) {
		const auto& parameters = node.def.parameters;
		size_t defaultParamCount = 0;
		for (size_t i = parameters.size(); i-- > 0; ) {
			const auto& param = parameters[i];
			if (param.defaultValue.has_value()) {
				CompileExpression(param.defaultValue.value(), instructions);
				defaultParamCount = parameters.size() - i;
			} else {
				break;
			}
		}

		Instruction def{};
		def.srcPos = node.srcPos;
		def.type = Instruction::Type::Def;
		def.def = std::make_unique<DefInstruction>();
		def.def->variables = std::vector<std::string>(
			node.def.variables.begin(),
			node.def.variables.end()
			);
		def.def->localCaptures = std::vector<std::string>(
			node.def.localCaptures.begin(),
			node.def.localCaptures.end()
			);
		def.def->globalCaptures = std::vector<std::string>(
			node.def.globalCaptures.begin(),
			node.def.globalCaptures.end()
			);
		def.def->instructions = MakeRcPtr<std::vector<Instruction>>();
		def.def->prettyName = node.def.name;
		def.def->defaultParameterCount = defaultParamCount;
		auto& params = def.def->parameters;
		params = std::move(node.def.parameters);
		if (!params.empty() && params.back().type == Parameter::Type::Kwargs) {
			def.def->kwArgs = std::move(params.back().name);
			params.pop_back();
		}
		if (!params.empty() && params.back().type == Parameter::Type::ListArgs) {
			def.def->listArgs = std::move(params.back().name);
			params.pop_back();
		}
		CompileBody(node.def.body, *def.def->instructions);
		instructions.push_back(std::move(def));
	}

	static void CompileDef(const Statement& node, std::vector<Instruction>& instructions) {
		auto& def = node.Get<stat::Def>().expr;
		CompileFunction(def, instructions);

		Instruction assign{};
		assign.srcPos = node.srcPos;
		assign.type = Instruction::Type::DirectAssign;
		assign.directAssign = std::make_unique<DirectAssignInstruction>();
		assign.directAssign->assignTarget.type = AssignType::Direct;
		assign.directAssign->assignTarget.direct = def.def.name;
		instructions.push_back(std::move(assign));

		Instruction pop{};
		pop.srcPos = node.srcPos;
		pop.type = Instruction::Type::Pop;
		instructions.push_back(std::move(pop));
	}

	static void CompileClass(const Statement& node, std::vector<Instruction>& instructions) {
		auto& klass = node.Get<stat::Class>();
		for (const auto& child :klass .body) {
			CompileDef(child, instructions);
			instructions.pop_back();
			instructions.pop_back();
			instructions.back().def->isMethod = true;
		}

		Instruction argFrame{};
		argFrame.srcPos = node.srcPos;
		argFrame.type = Instruction::Type::PushArgFrame;
		instructions.push_back(std::move(argFrame));

		for (const auto& base : klass.bases) {
			CompileExpression(base, instructions);
		}

		Instruction klassCreate{};
		klassCreate.srcPos = node.srcPos;
		klassCreate.type = Instruction::Type::Class;
		klassCreate.klass = std::make_unique<ClassInstruction>();
		klassCreate.klass->methodNames = klass.methodNames;
		klassCreate.klass->prettyName = klass.name;
		instructions.push_back(std::move(klassCreate));

		Instruction assign{};
		assign.srcPos = node.srcPos;
		assign.type = Instruction::Type::DirectAssign;
		assign.directAssign = std::make_unique<DirectAssignInstruction>();
		assign.directAssign->assignTarget.type = AssignType::Direct;
		assign.directAssign->assignTarget.direct = klass.name;
		instructions.push_back(std::move(assign));

		Instruction pop{};
		pop.srcPos = node.srcPos;
		pop.type = Instruction::Type::Pop;
		instructions.push_back(std::move(pop));
	}

	static void CompileImportFrom(const Statement& node, std::vector<Instruction>& instructions) {
		auto& importFrom = node.Get<stat::ImportFrom>();
		Instruction instr{};
		instr.srcPos = node.srcPos;
		instr.type = Instruction::Type::ImportFrom;
		instr.importFrom = std::make_unique<ImportFromInstruction>();
		instr.importFrom->module = importFrom.module;
		instr.importFrom->names = importFrom.names;
		instr.importFrom->alias = importFrom.alias;
		instructions.push_back(std::move(instr));
	}

	static void CompileImport(const Statement& node, std::vector<Instruction>& instructions) {
		auto& import = node.Get<stat::Import>();
		Instruction instr{};
		instr.srcPos = node.srcPos;
		instr.type = Instruction::Type::Import;
		instr.import = std::make_unique<ImportInstruction>();
		instr.import->module = import.module;
		instr.import->alias = import.alias;
		instructions.push_back(std::move(instr));
	}

	static void CompileRaise(const Statement& node, std::vector<Instruction>& instructions) {
		CompileExpression(node.Get<stat::Raise>().expr, instructions);

		Instruction raise{};
		raise.srcPos = node.srcPos;
		raise.type = Instruction::Type::Raise;
		instructions.push_back(std::move(raise));
	}

	static void CompileTry(const Statement& node, std::vector<Instruction>& instructions) {
		/* 
		 * [Try block]
		 * Push except location and finally location
		 * Run try body
		 * Queue jump to end with 1 finally queued
		 * 
		 * [Except block]
		 * Check exception type. If not match, jump to next except block
		 * Clear exception
		 * Run except body
		 * Queue jump to end with 1 finally queued
		 * 
		 * [Finally block]
		 * Pop except and finally location
		 * Run finally body
		 * Jump to next queued finally if it exists or go to queued jump
		 */

		auto& tr = node.Get<stat::Try>();

		std::vector<size_t> jumpToEndInstructs;
		auto jumpToFinally = [&] {
			jumpToEndInstructs.push_back(instructions.size());
			Instruction jump{};
			jump.srcPos = instructions.back().srcPos;
			jump.type = Instruction::Type::QueueJump;
			jump.queuedJump = std::make_unique<QueuedJumpInstruction>();
			jump.queuedJump->finallyCount = 1;
			instructions.push_back(std::move(jump));
		};
		
		// Try block
		size_t pushTryIndex = instructions.size();
		Instruction pushTry{};
		pushTry.srcPos = node.srcPos;
		pushTry.type = Instruction::Type::PushTry;
		pushTry.pushTry = std::make_unique<TryFrameInstruction>();
		instructions.push_back(std::move(pushTry));

		CompileBody(tr.body, instructions);

		jumpToFinally();

		// Except blocks		
		instructions[pushTryIndex].pushTry->exceptJump = instructions.size();
		for (const auto& exceptClause : tr.exceptBlocks) {
			auto& except = exceptClause.Get<stat::Except>();
			std::optional<size_t> jumpToNextExceptIndex;
			
			// Check exception type
			if (except.type) {
				Instruction argFrame{};
				argFrame.srcPos = exceptClause.srcPos;
				argFrame.type = Instruction::Type::PushArgFrame;
				instructions.push_back(std::move(argFrame));

				Instruction isInst{};
				isInst.srcPos = exceptClause.srcPos;
				isInst.type = Instruction::Type::IsInstance;
				instructions.push_back(std::move(isInst));

				Instruction curExcept{};
				curExcept.srcPos = exceptClause.srcPos;
				curExcept.type = Instruction::Type::CurrentException;
				instructions.push_back(std::move(curExcept));

				CompileExpression(except.type.value(), instructions);

				Instruction call{};
				call.srcPos = exceptClause.srcPos;
				call.type = Instruction::Type::Call;
				instructions.push_back(std::move(call));

				jumpToNextExceptIndex = instructions.size();
				Instruction jumpToNextExcept{};
				jumpToNextExcept.srcPos = exceptClause.srcPos;
				jumpToNextExcept.type = Instruction::Type::JumpIfFalsePop;
				jumpToNextExcept.jump = std::make_unique<JumpInstruction>();
				instructions.push_back(std::move(jumpToNextExcept));

				// Assign exception to variable
				if (!except.variable.empty()) {
					Instruction curExcept{};
					curExcept.srcPos = exceptClause.srcPos;
					curExcept.type = Instruction::Type::CurrentException;
					instructions.push_back(std::move(curExcept));

					Instruction assign{};
					assign.srcPos = exceptClause.srcPos;
					assign.type = Instruction::Type::DirectAssign;
					assign.directAssign = std::make_unique<DirectAssignInstruction>();
					assign.directAssign->assignTarget.type = AssignType::Direct;
					assign.directAssign->assignTarget.direct = except.variable;
					instructions.push_back(std::move(assign));

					Instruction pop{};
					pop.srcPos = exceptClause.srcPos;
					pop.type = Instruction::Type::Pop;
					instructions.push_back(std::move(pop));
				}
			}

			Instruction exc{};
			exc.srcPos = exceptClause.srcPos;
			exc.type = Instruction::Type::ClearException;
			instructions.push_back(std::move(exc));

			CompileBody(except.body, instructions);

			jumpToFinally();

			if (jumpToNextExceptIndex.has_value()) {
				instructions[jumpToNextExceptIndex.value()].jump->location = instructions.size();
			}
		}

		// Finally block
		instructions[pushTryIndex].pushTry->finallyJump = instructions.size();

		Instruction popTry{};
		popTry.srcPos = node.srcPos;
		popTry.type = Instruction::Type::PopTry;
		popTry.jump = std::make_unique<JumpInstruction>();
		instructions.push_back(std::move(popTry));

		CompileBody(tr.finallyBody, instructions);

		Instruction endFinally{};
		endFinally.srcPos = node.srcPos;
		endFinally.type = Instruction::Type::EndFinally;
		instructions.push_back(std::move(endFinally));

		for (size_t instrIndex : jumpToEndInstructs) {
			instructions[instrIndex].queuedJump->location = instructions.size();
		}
	}

	using CompileFn = void(*)(const Statement&, std::vector<Instruction>&);

	static const std::unordered_map<size_t, CompileFn> COMPILE_FUNCTIONS = {
		{ StatIndex<stat::Expr>(), CompileExpressionStatement },
		{ StatIndex<stat::If>(), CompileIf },
		{ StatIndex<stat::While>(), CompileWhile },
		{ StatIndex<stat::Break>(), CompileBreak },
		{ StatIndex<stat::Continue>(), CompileContinue },
		{ StatIndex<stat::Return>(), CompileReturn },
		{ StatIndex<stat::Def>(), CompileDef },
		{ StatIndex<stat::Class>(), CompileClass },
		{ StatIndex<stat::Try>(), CompileTry },
		{ StatIndex<stat::Raise>(), CompileRaise },
		{ StatIndex<stat::Import>(), CompileImport },
		{ StatIndex<stat::ImportFrom>(), CompileImportFrom },
		{ StatIndex<stat::Pass>(), [](auto, auto) {}},
		{ StatIndex<stat::Global>(), [](auto, auto) {}},
		{ StatIndex<stat::NonLocal>(), [](auto, auto) {}},
	};

	static void CompileStatement(const Statement& node, std::vector<Instruction>& instructions) {
		COMPILE_FUNCTIONS.at(node.data.index())(node, instructions);
	}

	static void CompileBody(const std::vector<Statement>& body, std::vector<Instruction>& instructions) {
		for (const auto& child : body) {
			CompileStatement(child, instructions);
		}
	}

	std::vector<Instruction> Compile(const stat::Root& parseTree) {
		std::vector<Instruction> instructions;
		CompileBody(parseTree.expr.def.body, instructions);

		return instructions;
	}

}
