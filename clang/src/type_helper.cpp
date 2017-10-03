#include "type_helper.hpp"

#include "clang/Tooling/Core/QualTypeNames.h"
#include "clang/AST/Type.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"

static CopyPtr<Template> handleTemplate(const clang::CXXRecordDecl *record,
	const clang::ClassTemplateSpecializationDecl *decl);
static void tryReadDefaultArgumentValue(Argument &arg, const clang::QualType &qt,
  clang::ASTContext &ctx, const clang::Expr *expr);
static bool tryReadStringConstructor(Argument &arg, const clang::CXXConstructExpr *expr);

Type TypeHelper::qualTypeToType(const clang::QualType &qt, clang::ASTContext &ctx) {
	Type type;
	qualTypeToType(type, qt, ctx);
	return type;
}

void TypeHelper::qualTypeToType(Type &target, const clang::QualType &qt, clang::ASTContext &ctx) {
	if (target.fullName.empty()) {
		target.fullName = clang::TypeName::getFullyQualifiedName(qt, ctx);
	}

	if (qt->isReferenceType() || qt->isPointerType()) {
		target.isReference = target.isReference || qt->isReferenceType();
		target.isMove = target.isMove || qt->isRValueReferenceType();
		target.pointer++;
		return qualTypeToType(target, qt->getPointeeType(), ctx); // Recurse
	}

	if (const auto *record = qt->getAsCXXRecordDecl()) {
		if (const auto *tmpl = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record)) {
			target.templ = handleTemplate(record, tmpl);
		}
	}

	// Not a reference or pointer.
	target.isConst = qt.isConstQualified();
	target.isVoid = qt->isVoidType();
	target.isBuiltin = qt->isBuiltinType();
	target.baseName = clang::TypeName::getFullyQualifiedName(qt.getUnqualifiedType(), ctx);
}

static CopyPtr<Template> handleTemplate(const clang::CXXRecordDecl *record, const clang::ClassTemplateSpecializationDecl *decl) {
	Template t;
	clang::ASTContext &ctx = decl->getASTContext();

	if (!record) return CopyPtr<Template>();

	const clang::Type *typePtr = record->getTypeForDecl();
	clang::QualType qt(typePtr, 0);
	t.baseName = record->getQualifiedNameAsString();
	t.fullName = clang::TypeName::getFullyQualifiedName(qt, ctx);

	for (const clang::TemplateArgument &argument : decl->getTemplateInstantiationArgs().asArray()) {

		// Sanity check, ignore whole template otherwise.
		if (argument.getKind() != clang::TemplateArgument::Type)
			return CopyPtr<Template>();

		Type type = TypeHelper::qualTypeToType(argument.getAsType(), ctx);
		t.arguments.push_back(type);
	}

	return CopyPtr<Template>(t);
}

Argument TypeHelper::processFunctionParameter(const clang::ParmVarDecl *decl) {
	clang::ASTContext &ctx = decl->getASTContext();
	Argument arg;

	clang::QualType qt = decl->getType();
	qualTypeToType(arg, qt, ctx);
	arg.name = decl->getQualifiedNameAsString();
	arg.hasDefault = decl->hasDefaultArg();
	arg.value = JsonStream::Null;

	// If the parameter has a default value, try to figure out this value.  Can
	// fail if e.g. the call has side-effects (Like calling another method).  Will
	// work for constant expressions though, like `true` or `3 + 5`.
	if (arg.hasDefault) {
		tryReadDefaultArgumentValue(arg, qt, ctx, decl->getDefaultArg());
	}

	return arg;
}

static bool describesStringClass(const clang::CXXConstructorDecl *ctorDecl) {
	std::string name = ctorDecl->getParent()->getQualifiedNameAsString();
	if (name == "std::__cxx11::basic_string" || name == "QString") {
		return true;
	} else {
		return false;
	}
}

static bool stringLiteralFromExpression(Argument &arg, const clang::Expr *expr) {
	if (const clang::MaterializeTemporaryExpr *argExpr = llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expr)) {
		return stringLiteralFromExpression(arg, argExpr->GetTemporaryExpr());
	} else if (const clang::CXXBindTemporaryExpr *bindExpr = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expr)) {
		return stringLiteralFromExpression(arg, bindExpr->getSubExpr());
	} else if (const clang::CastExpr *castExpr = llvm::dyn_cast<clang::CastExpr>(expr)) {
		return stringLiteralFromExpression(arg, castExpr->getSubExprAsWritten());
	} else if (const clang::CXXConstructExpr *ctorExpr = llvm::dyn_cast<clang::CXXConstructExpr>(expr)) {
		return tryReadStringConstructor(arg, ctorExpr);
	} else if (const clang::StringLiteral *strExpr = llvm::dyn_cast<clang::StringLiteral>(expr)) {
		// We found it!
		arg.value = strExpr->getString().str();
		return true;
	} else { // Failed to destructure.
		return false;
	}
}

static bool tryReadStringConstructor(Argument &arg, const clang::CXXConstructExpr *expr) {
	if (!describesStringClass(expr->getConstructor())) {
		return false;
	}

	// The constructor call needs to have no (= empty) or a single argument.
	if (expr->getNumArgs() == 0) { // This is an empty string!
		arg.value = std::string();
		return true;
	} else if (expr->getNumArgs() == 1) {
		return stringLiteralFromExpression(arg, expr->getArg(0));
	} else { // No rules for more than one argument.
		return false;
	}
}

static void tryReadDefaultArgumentValue(Argument &arg, const clang::QualType &qt,
  clang::ASTContext &ctx, const clang::Expr *expr) {
	clang::Expr::EvalResult result;

	if (!expr->EvaluateAsRValue(result, ctx)) {
		// Failed to evaluate - Try to unpack this expression
		stringLiteralFromExpression(arg, expr);
		return;
	}

	if (result.HasSideEffects || result.HasUndefinedBehavior) {
		return; // Don't accept if there are side-effects or undefined behaviour.
	}

	if (qt->isPointerType()) {
		// For a pointer-type, just store if it was `nullptr` (== true).
		arg.value = result.Val.isNullPointer();
	} else if (qt->isBooleanType()) {
		arg.value = result.Val.getInt().getBoolValue();
	} else if (qt->isIntegerType()) {
		const llvm::APSInt &v = result.Val.getInt();
		int64_t i64 = v.getExtValue();

		if (qt->isSignedIntegerType())
			arg.value = i64;
		else // Is there better way?
			arg.value = static_cast<uint64_t>(i64);
	} else if (qt->isFloatingType()) {
		arg.value = result.Val.getFloat().convertToDouble();
	}
}
