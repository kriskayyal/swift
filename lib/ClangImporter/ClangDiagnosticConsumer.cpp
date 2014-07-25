//===--- ClangDiagnosticConsumer.cpp - Handle Clang diagnostics -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ClangDiagnosticConsumer.h"
#include "ImporterImpl.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsClangImporter.h"
#include "clang/AST/ASTContext.h"
#include "clang/Frontend/DiagnosticRenderer.h"
#include "clang/Lex/LexDiagnostic.h"

using namespace swift;

namespace {
  class ClangDiagRenderer final : public clang::DiagnosticNoteRenderer {
    const std::function<void(clang::SourceLocation,
                             clang::DiagnosticsEngine::Level,
                             StringRef)> callback;

  public:
    template <typename Fn>
    ClangDiagRenderer(const clang::ASTContext &ctx, const Fn &fn)
       : DiagnosticNoteRenderer(ctx.getLangOpts(),
                                &ctx.getDiagnostics().getDiagnosticOptions()),
         callback(fn) {}

  private:
    void emitDiagnosticMessage(clang::SourceLocation Loc,
                               clang::PresumedLoc PLoc,
                               clang::DiagnosticsEngine::Level Level,
                               StringRef Message,
                               ArrayRef<clang::CharSourceRange> Ranges,
                               const clang::SourceManager *SM,
                               clang::DiagOrStoredDiag Info) override {
      if (SM->isInMainFile(Loc))
        return;
      if (StringRef(SM->getBufferName(Loc)) ==
          ClangImporter::Implementation::bridgingHeaderBufferName)
        return;
      callback(Loc, Level, Message);
    }

    void emitDiagnosticLoc(clang::SourceLocation Loc, clang::PresumedLoc PLoc,
                           clang::DiagnosticsEngine::Level Level,
                           ArrayRef<clang::CharSourceRange> Ranges,
                           const clang::SourceManager &SM) override {}

    void emitCodeContext(clang::SourceLocation Loc,
                         clang::DiagnosticsEngine::Level Level,
                         SmallVectorImpl<clang::CharSourceRange>& Ranges,
                         ArrayRef<clang::FixItHint> Hints,
                         const clang::SourceManager &SM) override {}

    void emitNote(clang::SourceLocation Loc, StringRef Message,
                  const clang::SourceManager *SM) override {
      // We get invalid note locations when trying to describe where a module
      // is imported and the actual location is in Swift.
      if (Loc.isInvalid())
        return;
      emitDiagnosticMessage(Loc, {}, clang::DiagnosticsEngine::Note, Message,
                            {}, SM, {});
    }
  };
}

ClangDiagnosticConsumer::ClangDiagnosticConsumer(
    ClangImporter::Implementation &impl,
    clang::DiagnosticOptions &clangDiagOptions,
    bool dumpToStderr)
  : TextDiagnosticPrinter(llvm::errs(), &clangDiagOptions),
    ImporterImpl(impl), DumpToStderr(dumpToStderr) {}

static SourceLoc findEndOfLine(SourceManager &SM, SourceLoc loc,
                               unsigned bufferID) {
  CharSourceRange entireBuffer = SM.getRangeForBuffer(bufferID);
  CharSourceRange rangeFromLoc{SM, loc, entireBuffer.getEnd()};
  StringRef textFromLoc = SM.extractText(rangeFromLoc);
  size_t newlineOffset = textFromLoc.find_first_of("\r\n");
  if (newlineOffset == StringRef::npos)
    return entireBuffer.getEnd();
  return loc.getAdvancedLoc(newlineOffset);
}

static SourceLoc resolveSourceLocation(const ASTContext &ctx,
                                       clang::SourceManager &clangSrcMgr,
                                       clang::SourceLocation clangLoc) {
  SourceLoc loc;

  clangLoc = clangSrcMgr.getFileLoc(clangLoc);
  auto decomposedLoc = clangSrcMgr.getDecomposedLoc(clangLoc);
  if (decomposedLoc.first.isInvalid())
    return loc;

  auto buffer = clangSrcMgr.getBuffer(decomposedLoc.first);

  auto mirrorID =
    ctx.SourceMgr.getIDForBufferIdentifier(buffer->getBufferIdentifier());
  if (!mirrorID) {
    std::unique_ptr<llvm::MemoryBuffer> mirrorBuffer{
      llvm::MemoryBuffer::getMemBuffer(buffer->getBuffer(),
                                       buffer->getBufferIdentifier(),
                                       /*nullTerminated=*/true)
    };
    mirrorID = ctx.SourceMgr.addNewSourceBuffer(std::move(mirrorBuffer));
  }
  loc = ctx.SourceMgr.getLocForOffset(mirrorID.getValue(),
                                      decomposedLoc.second);

  auto presumedLoc = clangSrcMgr.getPresumedLoc(clangLoc);
  if (!presumedLoc.getFilename())
    return loc;

  assert(presumedLoc.getLine() > 0 && "can't handle non-positive lines");
  unsigned bufferLineNumber =
    clangSrcMgr.getLineNumber(decomposedLoc.first, decomposedLoc.second);

  StringRef presumedFile = presumedLoc.getFilename();
  SourceLoc startOfLine = loc.getAdvancedLoc(-presumedLoc.getColumn() + 1);
  bool isNewVirtualFile =
    ctx.SourceMgr.openVirtualFile(startOfLine, presumedFile,
                                  presumedLoc.getLine() - bufferLineNumber);
  if (isNewVirtualFile) {
    SourceLoc endOfLine = findEndOfLine(ctx.SourceMgr, loc,
                                        mirrorID.getValue());
    ctx.SourceMgr.closeVirtualFile(endOfLine);
  }

  return loc;
}


void ClangDiagnosticConsumer::HandleDiagnostic(
    clang::DiagnosticsEngine::Level clangDiagLevel,
    const clang::Diagnostic &clangDiag) {
  // Handle the module-not-found diagnostic specially if it's a top-level module
  // we're looking for.
  if (clangDiag.getID() == clang::diag::err_module_not_found &&
      CurrentImport && clangDiag.getArgStdStr(0) == CurrentImport->getName()) {
    setCurrentImportMissing();
    return;
  }

  const ASTContext &ctx = ImporterImpl.SwiftContext;
  clang::SourceManager &clangSrcMgr = clangDiag.getSourceManager();

  if (clangDiag.getID() == clang::diag::err_module_not_built &&
      CurrentImport && clangDiag.getArgStdStr(0) == CurrentImport->getName()) {
    SourceLoc loc = resolveSourceLocation(ctx, clangSrcMgr,
                                          clangDiag.getLocation());

    ctx.Diags.diagnose(loc, diag::clang_cannot_build_module,
                       CurrentImport->getName());
    return;
  }

  // Satisfy the default implementation (bookkeeping).
  if (DumpToStderr)
    TextDiagnosticPrinter::HandleDiagnostic(clangDiagLevel, clangDiag);
  else
    DiagnosticConsumer::HandleDiagnostic(clangDiagLevel, clangDiag);

  // FIXME: Map over source ranges in the diagnostic.
  auto emitDiag = [&](clang::SourceLocation clangNoteLoc,
                      clang::DiagnosticsEngine::Level clangDiagLevel,
                      StringRef message) {
    decltype(diag::error_from_clang) diagKind;
    switch (clangDiagLevel) {
    case clang::DiagnosticsEngine::Ignored:
      return;
    case clang::DiagnosticsEngine::Note:
      diagKind = diag::note_from_clang;
      break;
    case clang::DiagnosticsEngine::Remark:
      // FIXME: We don't handle remarks yet.
      return;
    case clang::DiagnosticsEngine::Warning:
      diagKind = diag::warning_from_clang;
      break;
    case clang::DiagnosticsEngine::Error:
    case clang::DiagnosticsEngine::Fatal:
      // FIXME: What happens after a fatal error in the importer?
      diagKind = diag::error_from_clang;
      break;
    }

    SourceLoc noteLoc = resolveSourceLocation(ctx, clangSrcMgr, clangNoteLoc);
    ctx.Diags.diagnose(noteLoc, diagKind, message);
  };

  ClangDiagRenderer renderer(ImporterImpl.getClangASTContext(),
                             std::cref(emitDiag));

  llvm::SmallString<128> message;
  clangDiag.FormatDiagnostic(message);
  renderer.emitDiagnostic(clangDiag.getLocation(), clangDiagLevel, message,
                          clangDiag.getRanges(), clangDiag.getFixItHints(),
                          &clangSrcMgr);
}
