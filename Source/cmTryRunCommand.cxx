/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmTryRunCommand.h"

#include <cstdio>

#include <cm/optional>

#include "cmsys/FStream.hxx"

#include "cmArgumentParserTypes.h"
#include "cmCoreTryCompile.h"
#include "cmDuration.h"
#include "cmExecutionStatus.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmRange.h"
#include "cmState.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmValue.h"
#include "cmake.h"

namespace {

class TryRunCommandImpl : public cmCoreTryCompile
{
public:
  TryRunCommandImpl(cmMakefile* mf)
    : cmCoreTryCompile(mf)
  {
  }

  bool TryRunCode(std::vector<std::string> const& args);

  void RunExecutable(const std::string& runArgs,
                     cm::optional<std::string> const& workDir,
                     std::string* runOutputContents,
                     std::string* runOutputStdOutContents,
                     std::string* runOutputStdErrContents);
  void DoNotRunExecutable(const std::string& runArgs,
                          const std::string& srcFile,
                          std::string const& compileResultVariable,
                          std::string* runOutputContents,
                          std::string* runOutputStdOutContents,
                          std::string* runOutputStdErrContents);

  std::string RunResultVariable;
};

bool TryRunCommandImpl::TryRunCode(std::vector<std::string> const& argv)
{
  this->RunResultVariable = argv[0];
  cmCoreTryCompile::Arguments arguments =
    this->ParseArgs(cmMakeRange(argv).advance(1), true);
  if (!arguments) {
    return true;
  }

  // although they could be used together, don't allow it, because
  // using OUTPUT_VARIABLE makes crosscompiling harder
  if (arguments.OutputVariable &&
      (arguments.CompileOutputVariable || arguments.RunOutputVariable ||
       arguments.RunOutputStdOutVariable ||
       arguments.RunOutputStdErrVariable)) {
    cmSystemTools::Error(
      "You cannot use OUTPUT_VARIABLE together with COMPILE_OUTPUT_VARIABLE "
      ", RUN_OUTPUT_VARIABLE, RUN_OUTPUT_STDOUT_VARIABLE or "
      "RUN_OUTPUT_STDERR_VARIABLE. "
      "Please use only COMPILE_OUTPUT_VARIABLE, RUN_OUTPUT_VARIABLE, "
      "RUN_OUTPUT_STDOUT_VARIABLE "
      "and/or RUN_OUTPUT_STDERR_VARIABLE.");
    return false;
  }

  if ((arguments.RunOutputStdOutVariable ||
       arguments.RunOutputStdErrVariable) &&
      arguments.RunOutputVariable) {
    cmSystemTools::Error(
      "You cannot use RUN_OUTPUT_STDOUT_VARIABLE or "
      "RUN_OUTPUT_STDERR_VARIABLE together "
      "with RUN_OUTPUT_VARIABLE. Please use only COMPILE_OUTPUT_VARIABLE or "
      "RUN_OUTPUT_STDOUT_VARIABLE and/or RUN_OUTPUT_STDERR_VARIABLE.");
    return false;
  }

  if (arguments.RunWorkingDirectory) {
    if (!cmSystemTools::MakeDirectory(*arguments.RunWorkingDirectory)) {
      cmSystemTools::Error(cmStrCat("Error creating working directory \"",
                                    *arguments.RunWorkingDirectory, "\"."));
      return false;
    }
  }

  bool captureRunOutput = false;
  bool captureRunOutputStdOutErr = false;
  if (arguments.OutputVariable) {
    captureRunOutput = true;
  } else if (arguments.CompileOutputVariable) {
    arguments.OutputVariable = arguments.CompileOutputVariable;
  }
  if (arguments.RunOutputStdOutVariable || arguments.RunOutputStdErrVariable) {
    captureRunOutputStdOutErr = true;
  } else if (arguments.RunOutputVariable) {
    captureRunOutput = true;
  }

  // do the try compile
  bool compiled = this->TryCompileCode(arguments, cmStateEnums::EXECUTABLE);

  // now try running the command if it compiled
  if (compiled) {
    if (this->OutputFile.empty()) {
      cmSystemTools::Error(this->FindErrorMessage);
    } else {
      std::string runArgs;
      if (arguments.RunArgs) {
        runArgs = cmStrCat(" ", cmJoin(*arguments.RunArgs, " "));
      }

      // "run" it and capture the output
      std::string runOutputContents;
      std::string runOutputStdOutContents;
      std::string runOutputStdErrContents;
      if (this->Makefile->IsOn("CMAKE_CROSSCOMPILING") &&
          !this->Makefile->IsDefinitionSet("CMAKE_CROSSCOMPILING_EMULATOR")) {
        this->DoNotRunExecutable(
          runArgs, *arguments.SourceDirectoryOrFile,
          *arguments.CompileResultVariable,
          captureRunOutput ? &runOutputContents : nullptr,
          captureRunOutputStdOutErr && arguments.RunOutputStdOutVariable
            ? &runOutputStdOutContents
            : nullptr,
          captureRunOutputStdOutErr && arguments.RunOutputStdErrVariable
            ? &runOutputStdErrContents
            : nullptr);
      } else {
        this->RunExecutable(
          runArgs, arguments.RunWorkingDirectory,
          captureRunOutput ? &runOutputContents : nullptr,
          captureRunOutputStdOutErr && arguments.RunOutputStdOutVariable
            ? &runOutputStdOutContents
            : nullptr,
          captureRunOutputStdOutErr && arguments.RunOutputStdErrVariable
            ? &runOutputStdErrContents
            : nullptr);
      }

      // now put the output into the variables
      if (arguments.RunOutputVariable) {
        this->Makefile->AddDefinition(*arguments.RunOutputVariable,
                                      runOutputContents);
      }
      if (arguments.RunOutputStdOutVariable) {
        this->Makefile->AddDefinition(*arguments.RunOutputStdOutVariable,
                                      runOutputStdOutContents);
      }
      if (arguments.RunOutputStdErrVariable) {
        this->Makefile->AddDefinition(*arguments.RunOutputStdErrVariable,
                                      runOutputStdErrContents);
      }

      if (arguments.OutputVariable && !arguments.CompileOutputVariable) {
        // if the TryCompileCore saved output in this outputVariable then
        // prepend that output to this output
        cmValue compileOutput =
          this->Makefile->GetDefinition(*arguments.OutputVariable);
        if (compileOutput) {
          runOutputContents = *compileOutput + runOutputContents;
        }
        this->Makefile->AddDefinition(*arguments.OutputVariable,
                                      runOutputContents);
      }
    }
  }

  // if we created a directory etc, then cleanup after ourselves
  if (!this->Makefile->GetCMakeInstance()->GetDebugTryCompile()) {
    this->CleanupFiles(this->BinaryDirectory);
  }
  return true;
}

void TryRunCommandImpl::RunExecutable(const std::string& runArgs,
                                      cm::optional<std::string> const& workDir,
                                      std::string* out, std::string* stdOut,
                                      std::string* stdErr)
{
  int retVal = -1;

  std::string finalCommand;
  const std::string& emulator =
    this->Makefile->GetSafeDefinition("CMAKE_CROSSCOMPILING_EMULATOR");
  if (!emulator.empty()) {
    std::vector<std::string> emulatorWithArgs = cmExpandedList(emulator);
    finalCommand +=
      cmSystemTools::ConvertToRunCommandPath(emulatorWithArgs[0]);
    finalCommand += " ";
    for (std::string const& arg : cmMakeRange(emulatorWithArgs).advance(1)) {
      finalCommand += "\"";
      finalCommand += arg;
      finalCommand += "\"";
      finalCommand += " ";
    }
  }
  finalCommand += cmSystemTools::ConvertToRunCommandPath(this->OutputFile);
  if (!runArgs.empty()) {
    finalCommand += runArgs;
  }
  bool worked = cmSystemTools::RunSingleCommand(
    finalCommand, stdOut || stdErr ? stdOut : out,
    stdOut || stdErr ? stdErr : out, &retVal,
    workDir ? workDir->c_str() : nullptr, cmSystemTools::OUTPUT_NONE,
    cmDuration::zero());
  // set the run var
  char retChar[16];
  const char* retStr;
  if (worked) {
    snprintf(retChar, sizeof(retChar), "%i", retVal);
    retStr = retChar;
  } else {
    retStr = "FAILED_TO_RUN";
  }
  this->Makefile->AddCacheDefinition(this->RunResultVariable, retStr,
                                     "Result of try_run()",
                                     cmStateEnums::INTERNAL);
}

/* This is only used when cross compiling. Instead of running the
 executable, two cache variables are created which will hold the results
 the executable would have produced.
*/
void TryRunCommandImpl::DoNotRunExecutable(
  const std::string& runArgs, const std::string& srcFile,
  std::string const& compileResultVariable, std::string* out,
  std::string* stdOut, std::string* stdErr)
{
  // copy the executable out of the CMakeFiles/ directory, so it is not
  // removed at the end of try_run() and the user can run it manually
  // on the target platform.
  std::string copyDest =
    cmStrCat(this->Makefile->GetHomeOutputDirectory(), "/CMakeFiles/",
             cmSystemTools::GetFilenameWithoutExtension(this->OutputFile), '-',
             this->RunResultVariable,
             cmSystemTools::GetFilenameExtension(this->OutputFile));
  cmSystemTools::CopyFileAlways(this->OutputFile, copyDest);

  std::string resultFileName =
    cmStrCat(this->Makefile->GetHomeOutputDirectory(), "/TryRunResults.cmake");

  std::string detailsString = cmStrCat("For details see ", resultFileName);

  std::string internalRunOutputName =
    this->RunResultVariable + "__TRYRUN_OUTPUT";
  std::string internalRunOutputStdOutName =
    this->RunResultVariable + "__TRYRUN_OUTPUT_STDOUT";
  std::string internalRunOutputStdErrName =
    this->RunResultVariable + "__TRYRUN_OUTPUT_STDERR";
  bool error = false;

  if (!this->Makefile->GetDefinition(this->RunResultVariable)) {
    // if the variables doesn't exist, create it with a helpful error text
    // and mark it as advanced
    std::string comment =
      cmStrCat("Run result of try_run(), indicates whether the executable "
               "would have been able to run on its target platform.\n",
               detailsString);
    this->Makefile->AddCacheDefinition(this->RunResultVariable,
                                       "PLEASE_FILL_OUT-FAILED_TO_RUN",
                                       comment.c_str(), cmStateEnums::STRING);

    cmState* state = this->Makefile->GetState();
    cmValue existingValue = state->GetCacheEntryValue(this->RunResultVariable);
    if (existingValue) {
      state->SetCacheEntryProperty(this->RunResultVariable, "ADVANCED", "1");
    }

    error = true;
  }

  // is the output from the executable used ?
  if (stdOut || stdErr) {
    if (!this->Makefile->GetDefinition(internalRunOutputStdOutName)) {
      // if the variables doesn't exist, create it with a helpful error text
      // and mark it as advanced
      std::string comment = cmStrCat(
        "Output of try_run(), contains the text, which the executable "
        "would have printed on stdout on its target platform.\n",
        detailsString);

      this->Makefile->AddCacheDefinition(
        internalRunOutputStdOutName, "PLEASE_FILL_OUT-NOTFOUND",
        comment.c_str(), cmStateEnums::STRING);
      cmState* state = this->Makefile->GetState();
      cmValue existing =
        state->GetCacheEntryValue(internalRunOutputStdOutName);
      if (existing) {
        state->SetCacheEntryProperty(internalRunOutputStdOutName, "ADVANCED",
                                     "1");
      }

      error = true;
    }

    if (!this->Makefile->GetDefinition(internalRunOutputStdErrName)) {
      // if the variables doesn't exist, create it with a helpful error text
      // and mark it as advanced
      std::string comment = cmStrCat(
        "Output of try_run(), contains the text, which the executable "
        "would have printed on stderr on its target platform.\n",
        detailsString);

      this->Makefile->AddCacheDefinition(
        internalRunOutputStdErrName, "PLEASE_FILL_OUT-NOTFOUND",
        comment.c_str(), cmStateEnums::STRING);
      cmState* state = this->Makefile->GetState();
      cmValue existing =
        state->GetCacheEntryValue(internalRunOutputStdErrName);
      if (existing) {
        state->SetCacheEntryProperty(internalRunOutputStdErrName, "ADVANCED",
                                     "1");
      }

      error = true;
    }
  } else if (out) {
    if (!this->Makefile->GetDefinition(internalRunOutputName)) {
      // if the variables doesn't exist, create it with a helpful error text
      // and mark it as advanced
      std::string comment = cmStrCat(
        "Output of try_run(), contains the text, which the executable "
        "would have printed on stdout and stderr on its target platform.\n",
        detailsString);

      this->Makefile->AddCacheDefinition(
        internalRunOutputName, "PLEASE_FILL_OUT-NOTFOUND", comment.c_str(),
        cmStateEnums::STRING);
      cmState* state = this->Makefile->GetState();
      cmValue existing = state->GetCacheEntryValue(internalRunOutputName);
      if (existing) {
        state->SetCacheEntryProperty(internalRunOutputName, "ADVANCED", "1");
      }

      error = true;
    }
  }

  if (error) {
    static bool firstTryRun = true;
    cmsys::ofstream file(resultFileName.c_str(),
                         firstTryRun ? std::ios::out : std::ios::app);
    if (file) {
      if (firstTryRun) {
        /* clang-format off */
        file << "# This file was generated by CMake because it detected "
                "try_run() commands\n"
                "# in crosscompiling mode. It will be overwritten by the next "
                "CMake run.\n"
                "# Copy it to a safe location, set the variables to "
                "appropriate values\n"
                "# and use it then to preset the CMake cache (using -C).\n\n";
        /* clang-format on */
      }

      std::string comment =
        cmStrCat('\n', this->RunResultVariable,
                 "\n   indicates whether the executable would have been able "
                 "to run on its\n"
                 "   target platform. If so, set ",
                 this->RunResultVariable,
                 " to\n"
                 "   the exit code (in many cases 0 for success), otherwise "
                 "enter \"FAILED_TO_RUN\".\n");
      if (stdOut || stdErr) {
        if (stdOut) {
          comment += internalRunOutputStdOutName;
          comment +=
            "\n   contains the text the executable "
            "would have printed on stdout.\n"
            "   If the executable would not have been able to run, set ";
          comment += internalRunOutputStdOutName;
          comment += " empty.\n"
                     "   Otherwise check if the output is evaluated by the "
                     "calling CMake code. If so,\n"
                     "   check what the source file would have printed when "
                     "called with the given arguments.\n";
        }
        if (stdErr) {
          comment += internalRunOutputStdErrName;
          comment +=
            "\n   contains the text the executable "
            "would have printed on stderr.\n"
            "   If the executable would not have been able to run, set ";
          comment += internalRunOutputStdErrName;
          comment += " empty.\n"
                     "   Otherwise check if the output is evaluated by the "
                     "calling CMake code. If so,\n"
                     "   check what the source file would have printed when "
                     "called with the given arguments.\n";
        }
      } else if (out) {
        comment += internalRunOutputName;
        comment +=
          "\n   contains the text the executable "
          "would have printed on stdout and stderr.\n"
          "   If the executable would not have been able to run, set ";
        comment += internalRunOutputName;
        comment += " empty.\n"
                   "   Otherwise check if the output is evaluated by the "
                   "calling CMake code. If so,\n"
                   "   check what the source file would have printed when "
                   "called with the given arguments.\n";
      }

      comment += "The ";
      comment += compileResultVariable;
      comment += " variable holds the build result for this try_run().\n\n"
                 "Source file   : ";
      comment += srcFile + "\n";
      comment += "Executable    : ";
      comment += copyDest + "\n";
      comment += "Run arguments : ";
      comment += runArgs;
      comment += "\n";
      comment += "   Called from: " + this->Makefile->FormatListFileStack();
      cmsys::SystemTools::ReplaceString(comment, "\n", "\n# ");
      file << comment << "\n\n";

      file << "set( " << this->RunResultVariable << " \n     \""
           << this->Makefile->GetSafeDefinition(this->RunResultVariable)
           << "\"\n     CACHE STRING \"Result from try_run\" FORCE)\n\n";

      if (out) {
        file << "set( " << internalRunOutputName << " \n     \""
             << this->Makefile->GetSafeDefinition(internalRunOutputName)
             << "\"\n     CACHE STRING \"Output from try_run\" FORCE)\n\n";
      }
      file.close();
    }
    firstTryRun = false;

    std::string errorMessage =
      cmStrCat("try_run() invoked in cross-compiling mode, "
               "please set the following cache variables "
               "appropriately:\n   ",
               this->RunResultVariable, " (advanced)\n");
    if (out) {
      errorMessage += "   " + internalRunOutputName + " (advanced)\n";
    }
    errorMessage += detailsString;
    cmSystemTools::Error(errorMessage);
    return;
  }

  if (stdOut || stdErr) {
    if (stdOut) {
      (*stdOut) = *this->Makefile->GetDefinition(internalRunOutputStdOutName);
    }
    if (stdErr) {
      (*stdErr) = *this->Makefile->GetDefinition(internalRunOutputStdErrName);
    }
  } else if (out) {
    (*out) = *this->Makefile->GetDefinition(internalRunOutputName);
  }
}
}

bool cmTryRunCommand(std::vector<std::string> const& args,
                     cmExecutionStatus& status)
{
  cmMakefile& mf = status.GetMakefile();

  if (args.size() < 4) {
    mf.IssueMessage(MessageType::FATAL_ERROR,
                    "The try_run() command requires at least 4 arguments.");
    return false;
  }

  if (mf.GetCMakeInstance()->GetWorkingMode() == cmake::FIND_PACKAGE_MODE) {
    mf.IssueMessage(
      MessageType::FATAL_ERROR,
      "The try_run() command is not supported in --find-package mode.");
    return false;
  }

  TryRunCommandImpl tr(&mf);
  return tr.TryRunCode(args);
}
