//===-- CommandObjectBreakpoint.cpp ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CommandObjectBreakpoint.h"
#include "CommandObjectBreakpointCommand.h"
#include "lldb/Breakpoint/Breakpoint.h"
#include "lldb/Breakpoint/BreakpointIDList.h"
#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionArgParser.h"
#include "lldb/Interpreter/OptionGroupPythonClassWithDict.h"
#include "lldb/Interpreter/OptionValueBoolean.h"
#include "lldb/Interpreter/OptionValueFileColonLine.h"
#include "lldb/Interpreter/OptionValueString.h"
#include "lldb/Interpreter/OptionValueUInt64.h"
#include "lldb/Interpreter/Options.h"
#include "lldb/Target/Language.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/ThreadSpec.h"
#include "lldb/Utility/RegularExpression.h"
#include "lldb/Utility/StreamString.h"

#include <memory>
#include <vector>

using namespace lldb;
using namespace lldb_private;

static void AddBreakpointDescription(Stream *s, Breakpoint *bp,
                                     lldb::DescriptionLevel level) {
  s->IndentMore();
  bp->GetDescription(s, level, true);
  s->IndentLess();
  s->EOL();
}

// Modifiable Breakpoint Options
#pragma mark Modify::CommandOptions
#define LLDB_OPTIONS_breakpoint_modify
#include "CommandOptions.inc"

class lldb_private::BreakpointOptionGroup : public OptionGroup {
public:
  BreakpointOptionGroup() : OptionGroup(), m_bp_opts(false) {}

  ~BreakpointOptionGroup() override = default;

  llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
    return llvm::makeArrayRef(g_breakpoint_modify_options);
  }

  Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                        ExecutionContext *execution_context) override {
    Status error;
    const int short_option =
        g_breakpoint_modify_options[option_idx].short_option;

    switch (short_option) {
    case 'c':
      // Normally an empty breakpoint condition marks is as unset. But we need
      // to say it was passed in.
      m_bp_opts.SetCondition(option_arg.str().c_str());
      m_bp_opts.m_set_flags.Set(BreakpointOptions::eCondition);
      break;
    case 'C':
      m_commands.push_back(std::string(option_arg));
      break;
    case 'd':
      m_bp_opts.SetEnabled(false);
      break;
    case 'e':
      m_bp_opts.SetEnabled(true);
      break;
    case 'G': {
      bool value, success;
      value = OptionArgParser::ToBoolean(option_arg, false, &success);
      if (success) {
        m_bp_opts.SetAutoContinue(value);
      } else
        error.SetErrorStringWithFormat(
            "invalid boolean value '%s' passed for -G option",
            option_arg.str().c_str());
    } break;
    case 'i': {
      uint32_t ignore_count;
      if (option_arg.getAsInteger(0, ignore_count))
        error.SetErrorStringWithFormat("invalid ignore count '%s'",
                                       option_arg.str().c_str());
      else
        m_bp_opts.SetIgnoreCount(ignore_count);
    } break;
    case 'o': {
      bool value, success;
      value = OptionArgParser::ToBoolean(option_arg, false, &success);
      if (success) {
        m_bp_opts.SetOneShot(value);
      } else
        error.SetErrorStringWithFormat(
            "invalid boolean value '%s' passed for -o option",
            option_arg.str().c_str());
    } break;
    case 't': {
      lldb::tid_t thread_id = LLDB_INVALID_THREAD_ID;
      if (option_arg[0] != '\0') {
        if (option_arg.getAsInteger(0, thread_id))
          error.SetErrorStringWithFormat("invalid thread id string '%s'",
                                         option_arg.str().c_str());
      }
      m_bp_opts.SetThreadID(thread_id);
    } break;
    case 'T':
      m_bp_opts.GetThreadSpec()->SetName(option_arg.str().c_str());
      break;
    case 'q':
      m_bp_opts.GetThreadSpec()->SetQueueName(option_arg.str().c_str());
      break;
    case 'x': {
      uint32_t thread_index = UINT32_MAX;
      if (option_arg[0] != '\n') {
        if (option_arg.getAsInteger(0, thread_index))
          error.SetErrorStringWithFormat("invalid thread index string '%s'",
                                         option_arg.str().c_str());
      }
      m_bp_opts.GetThreadSpec()->SetIndex(thread_index);
    } break;
    default:
      llvm_unreachable("Unimplemented option");
    }

    return error;
  }

  void OptionParsingStarting(ExecutionContext *execution_context) override {
    m_bp_opts.Clear();
    m_commands.clear();
  }

  Status OptionParsingFinished(ExecutionContext *execution_context) override {
    if (!m_commands.empty()) {
      auto cmd_data = std::make_unique<BreakpointOptions::CommandData>();

      for (std::string &str : m_commands)
        cmd_data->user_source.AppendString(str);

      cmd_data->stop_on_error = true;
      m_bp_opts.SetCommandDataCallback(cmd_data);
    }
    return Status();
  }

  const BreakpointOptions &GetBreakpointOptions() { return m_bp_opts; }

  std::vector<std::string> m_commands;
  BreakpointOptions m_bp_opts;
};

#define LLDB_OPTIONS_breakpoint_dummy
#include "CommandOptions.inc"

class BreakpointDummyOptionGroup : public OptionGroup {
public:
  BreakpointDummyOptionGroup() : OptionGroup() {}

  ~BreakpointDummyOptionGroup() override = default;

  llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
    return llvm::makeArrayRef(g_breakpoint_dummy_options);
  }

  Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                        ExecutionContext *execution_context) override {
    Status error;
    const int short_option =
        g_breakpoint_dummy_options[option_idx].short_option;

    switch (short_option) {
    case 'D':
      m_use_dummy = true;
      break;
    default:
      llvm_unreachable("Unimplemented option");
    }

    return error;
  }

  void OptionParsingStarting(ExecutionContext *execution_context) override {
    m_use_dummy = false;
  }

  bool m_use_dummy;
};

#define LLDB_OPTIONS_breakpoint_set
#include "CommandOptions.inc"

// CommandObjectBreakpointSet

class CommandObjectBreakpointSet : public CommandObjectParsed {
public:
  enum BreakpointSetType {
    eSetTypeInvalid,
    eSetTypeFileAndLine,
    eSetTypeAddress,
    eSetTypeFunctionName,
    eSetTypeFunctionRegexp,
    eSetTypeSourceRegexp,
    eSetTypeException,
    eSetTypeScripted,
  };

  CommandObjectBreakpointSet(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "breakpoint set",
            "Sets a breakpoint or set of breakpoints in the executable.",
            "breakpoint set <cmd-options>"),
        m_bp_opts(), m_python_class_options("scripted breakpoint", true, 'P'),
        m_options() {
    // We're picking up all the normal options, commands and disable.
    m_all_options.Append(&m_python_class_options,
                         LLDB_OPT_SET_1 | LLDB_OPT_SET_2, LLDB_OPT_SET_11);
    m_all_options.Append(&m_bp_opts,
                         LLDB_OPT_SET_1 | LLDB_OPT_SET_3 | LLDB_OPT_SET_4,
                         LLDB_OPT_SET_ALL);
    m_all_options.Append(&m_dummy_options, LLDB_OPT_SET_1, LLDB_OPT_SET_ALL);
    m_all_options.Append(&m_options);
    m_all_options.Finalize();
  }

  ~CommandObjectBreakpointSet() override = default;

  Options *GetOptions() override { return &m_all_options; }

  class CommandOptions : public OptionGroup {
  public:
    CommandOptions()
        : OptionGroup(), m_condition(), m_filenames(), m_func_names(),
          m_func_regexp(), m_source_text_regexp(), m_modules() {}

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option =
          g_breakpoint_set_options[option_idx].short_option;

      switch (short_option) {
      case 'a': {
        m_load_addr = OptionArgParser::ToAddress(execution_context, option_arg,
                                                 LLDB_INVALID_ADDRESS, &error);
      } break;

      case 'A':
        m_all_files = true;
        break;

      case 'b':
        m_func_names.push_back(std::string(option_arg));
        m_func_name_type_mask |= eFunctionNameTypeBase;
        break;

      case 'u':
        if (option_arg.getAsInteger(0, m_column))
          error.SetErrorStringWithFormat("invalid column number: %s",
                                         option_arg.str().c_str());
        break;

      case 'E': {
        LanguageType language = Language::GetLanguageTypeFromString(option_arg);

        switch (language) {
        case eLanguageTypeC89:
        case eLanguageTypeC:
        case eLanguageTypeC99:
        case eLanguageTypeC11:
          m_exception_language = eLanguageTypeC;
          break;
        case eLanguageTypeC_plus_plus:
        case eLanguageTypeC_plus_plus_03:
        case eLanguageTypeC_plus_plus_11:
        case eLanguageTypeC_plus_plus_14:
          m_exception_language = eLanguageTypeC_plus_plus;
          break;
        case eLanguageTypeObjC:
          m_exception_language = eLanguageTypeObjC;
          break;
        case eLanguageTypeObjC_plus_plus:
          error.SetErrorStringWithFormat(
              "Set exception breakpoints separately for c++ and objective-c");
          break;
        case eLanguageTypeUnknown:
          error.SetErrorStringWithFormat(
              "Unknown language type: '%s' for exception breakpoint",
              option_arg.str().c_str());
          break;
        default:
          error.SetErrorStringWithFormat(
              "Unsupported language type: '%s' for exception breakpoint",
              option_arg.str().c_str());
        }
      } break;

      case 'f':
        m_filenames.AppendIfUnique(FileSpec(option_arg));
        break;

      case 'F':
        m_func_names.push_back(std::string(option_arg));
        m_func_name_type_mask |= eFunctionNameTypeFull;
        break;

      case 'h': {
        bool success;
        m_catch_bp = OptionArgParser::ToBoolean(option_arg, true, &success);
        if (!success)
          error.SetErrorStringWithFormat(
              "Invalid boolean value for on-catch option: '%s'",
              option_arg.str().c_str());
      } break;

      case 'H':
        m_hardware = true;
        break;

      case 'K': {
        bool success;
        bool value;
        value = OptionArgParser::ToBoolean(option_arg, true, &success);
        if (value)
          m_skip_prologue = eLazyBoolYes;
        else
          m_skip_prologue = eLazyBoolNo;

        if (!success)
          error.SetErrorStringWithFormat(
              "Invalid boolean value for skip prologue option: '%s'",
              option_arg.str().c_str());
      } break;

      case 'l':
        if (option_arg.getAsInteger(0, m_line_num))
          error.SetErrorStringWithFormat("invalid line number: %s.",
                                         option_arg.str().c_str());
        break;

      case 'L':
        m_language = Language::GetLanguageTypeFromString(option_arg);
        if (m_language == eLanguageTypeUnknown)
          error.SetErrorStringWithFormat(
              "Unknown language type: '%s' for breakpoint",
              option_arg.str().c_str());
        break;

      case 'm': {
        bool success;
        bool value;
        value = OptionArgParser::ToBoolean(option_arg, true, &success);
        if (value)
          m_move_to_nearest_code = eLazyBoolYes;
        else
          m_move_to_nearest_code = eLazyBoolNo;

        if (!success)
          error.SetErrorStringWithFormat(
              "Invalid boolean value for move-to-nearest-code option: '%s'",
              option_arg.str().c_str());
        break;
      }

      case 'M':
        m_func_names.push_back(std::string(option_arg));
        m_func_name_type_mask |= eFunctionNameTypeMethod;
        break;

      case 'n':
        m_func_names.push_back(std::string(option_arg));
        m_func_name_type_mask |= eFunctionNameTypeAuto;
        break;

      case 'N': {
        if (BreakpointID::StringIsBreakpointName(option_arg, error))
          m_breakpoint_names.push_back(std::string(option_arg));
        else
          error.SetErrorStringWithFormat("Invalid breakpoint name: %s",
                                         option_arg.str().c_str());
        break;
      }

      case 'R': {
        lldb::addr_t tmp_offset_addr;
        tmp_offset_addr = OptionArgParser::ToAddress(execution_context,
                                                     option_arg, 0, &error);
        if (error.Success())
          m_offset_addr = tmp_offset_addr;
      } break;

      case 'O':
        m_exception_extra_args.AppendArgument("-O");
        m_exception_extra_args.AppendArgument(option_arg);
        break;

      case 'p':
        m_source_text_regexp.assign(std::string(option_arg));
        break;

      case 'r':
        m_func_regexp.assign(std::string(option_arg));
        break;

      case 's':
        m_modules.AppendIfUnique(FileSpec(option_arg));
        break;

      case 'S':
        m_func_names.push_back(std::string(option_arg));
        m_func_name_type_mask |= eFunctionNameTypeSelector;
        break;

      case 'w': {
        bool success;
        m_throw_bp = OptionArgParser::ToBoolean(option_arg, true, &success);
        if (!success)
          error.SetErrorStringWithFormat(
              "Invalid boolean value for on-throw option: '%s'",
              option_arg.str().c_str());
      } break;

      case 'X':
        m_source_regex_func_names.insert(std::string(option_arg));
        break;
        
      case 'y':
      {
        OptionValueFileColonLine value;
        Status fcl_err = value.SetValueFromString(option_arg);
        if (!fcl_err.Success()) {
          error.SetErrorStringWithFormat(
              "Invalid value for file:line specifier: %s",
              fcl_err.AsCString());
        } else {
          m_filenames.AppendIfUnique(value.GetFileSpec());
          m_line_num = value.GetLineNumber();
          m_column = value.GetColumnNumber();
        }
      } break;
      
      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_filenames.Clear();
      m_line_num = 0;
      m_column = 0;
      m_func_names.clear();
      m_func_name_type_mask = eFunctionNameTypeNone;
      m_func_regexp.clear();
      m_source_text_regexp.clear();
      m_modules.Clear();
      m_load_addr = LLDB_INVALID_ADDRESS;
      m_offset_addr = 0;
      m_catch_bp = false;
      m_throw_bp = true;
      m_hardware = false;
      m_exception_language = eLanguageTypeUnknown;
      m_language = lldb::eLanguageTypeUnknown;
      m_skip_prologue = eLazyBoolCalculate;
      m_breakpoint_names.clear();
      m_all_files = false;
      m_exception_extra_args.Clear();
      m_move_to_nearest_code = eLazyBoolCalculate;
      m_source_regex_func_names.clear();
      m_current_key.clear();
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::makeArrayRef(g_breakpoint_set_options);
    }

    // Instance variables to hold the values for command options.

    std::string m_condition;
    FileSpecList m_filenames;
    uint32_t m_line_num = 0;
    uint32_t m_column = 0;
    std::vector<std::string> m_func_names;
    std::vector<std::string> m_breakpoint_names;
    lldb::FunctionNameType m_func_name_type_mask = eFunctionNameTypeNone;
    std::string m_func_regexp;
    std::string m_source_text_regexp;
    FileSpecList m_modules;
    lldb::addr_t m_load_addr = 0;
    lldb::addr_t m_offset_addr;
    bool m_catch_bp = false;
    bool m_throw_bp = true;
    bool m_hardware = false; // Request to use hardware breakpoints
    lldb::LanguageType m_exception_language = eLanguageTypeUnknown;
    lldb::LanguageType m_language = lldb::eLanguageTypeUnknown;
    LazyBool m_skip_prologue = eLazyBoolCalculate;
    bool m_all_files = false;
    Args m_exception_extra_args;
    LazyBool m_move_to_nearest_code = eLazyBoolCalculate;
    std::unordered_set<std::string> m_source_regex_func_names;
    std::string m_current_key;
  };

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target = GetSelectedOrDummyTarget(m_dummy_options.m_use_dummy);

    // The following are the various types of breakpoints that could be set:
    //   1).  -f -l -p  [-s -g]   (setting breakpoint by source location)
    //   2).  -a  [-s -g]         (setting breakpoint by address)
    //   3).  -n  [-s -g]         (setting breakpoint by function name)
    //   4).  -r  [-s -g]         (setting breakpoint by function name regular
    //   expression)
    //   5).  -p -f               (setting a breakpoint by comparing a reg-exp
    //   to source text)
    //   6).  -E [-w -h]          (setting a breakpoint for exceptions for a
    //   given language.)

    BreakpointSetType break_type = eSetTypeInvalid;

    if (!m_python_class_options.GetName().empty())
      break_type = eSetTypeScripted;
    else if (m_options.m_line_num != 0)
      break_type = eSetTypeFileAndLine;
    else if (m_options.m_load_addr != LLDB_INVALID_ADDRESS)
      break_type = eSetTypeAddress;
    else if (!m_options.m_func_names.empty())
      break_type = eSetTypeFunctionName;
    else if (!m_options.m_func_regexp.empty())
      break_type = eSetTypeFunctionRegexp;
    else if (!m_options.m_source_text_regexp.empty())
      break_type = eSetTypeSourceRegexp;
    else if (m_options.m_exception_language != eLanguageTypeUnknown)
      break_type = eSetTypeException;

    BreakpointSP bp_sp = nullptr;
    FileSpec module_spec;
    const bool internal = false;

    // If the user didn't specify skip-prologue, having an offset should turn
    // that off.
    if (m_options.m_offset_addr != 0 &&
        m_options.m_skip_prologue == eLazyBoolCalculate)
      m_options.m_skip_prologue = eLazyBoolNo;

    switch (break_type) {
    case eSetTypeFileAndLine: // Breakpoint by source position
    {
      FileSpec file;
      const size_t num_files = m_options.m_filenames.GetSize();
      if (num_files == 0) {
        if (!GetDefaultFile(target, file, result)) {
          result.AppendError("No file supplied and no default file available.");
          result.SetStatus(eReturnStatusFailed);
          return false;
        }
      } else if (num_files > 1) {
        result.AppendError("Only one file at a time is allowed for file and "
                           "line breakpoints.");
        result.SetStatus(eReturnStatusFailed);
        return false;
      } else
        file = m_options.m_filenames.GetFileSpecAtIndex(0);

      // Only check for inline functions if
      LazyBool check_inlines = eLazyBoolCalculate;

      bp_sp = target.CreateBreakpoint(
          &(m_options.m_modules), file, m_options.m_line_num,
          m_options.m_column, m_options.m_offset_addr, check_inlines,
          m_options.m_skip_prologue, internal, m_options.m_hardware,
          m_options.m_move_to_nearest_code);
    } break;

    case eSetTypeAddress: // Breakpoint by address
    {
      // If a shared library has been specified, make an lldb_private::Address
      // with the library, and use that.  That way the address breakpoint
      //  will track the load location of the library.
      size_t num_modules_specified = m_options.m_modules.GetSize();
      if (num_modules_specified == 1) {
        const FileSpec *file_spec =
            m_options.m_modules.GetFileSpecPointerAtIndex(0);
        bp_sp = target.CreateAddressInModuleBreakpoint(
            m_options.m_load_addr, internal, file_spec, m_options.m_hardware);
      } else if (num_modules_specified == 0) {
        bp_sp = target.CreateBreakpoint(m_options.m_load_addr, internal,
                                        m_options.m_hardware);
      } else {
        result.AppendError("Only one shared library can be specified for "
                           "address breakpoints.");
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
      break;
    }
    case eSetTypeFunctionName: // Breakpoint by function name
    {
      FunctionNameType name_type_mask = m_options.m_func_name_type_mask;

      if (name_type_mask == 0)
        name_type_mask = eFunctionNameTypeAuto;

      bp_sp = target.CreateBreakpoint(
          &(m_options.m_modules), &(m_options.m_filenames),
          m_options.m_func_names, name_type_mask, m_options.m_language,
          m_options.m_offset_addr, m_options.m_skip_prologue, internal,
          m_options.m_hardware);
    } break;

    case eSetTypeFunctionRegexp: // Breakpoint by regular expression function
                                 // name
    {
      RegularExpression regexp(m_options.m_func_regexp);
      if (llvm::Error err = regexp.GetError()) {
        result.AppendErrorWithFormat(
            "Function name regular expression could not be compiled: %s",
            llvm::toString(std::move(err)).c_str());
        // Check if the incorrect regex looks like a globbing expression and
        // warn the user about it.
        if (!m_options.m_func_regexp.empty()) {
          if (m_options.m_func_regexp[0] == '*' ||
              m_options.m_func_regexp[0] == '?')
            result.AppendWarning(
                "Function name regex does not accept glob patterns.");
        }
        result.SetStatus(eReturnStatusFailed);
        return false;
      }

      bp_sp = target.CreateFuncRegexBreakpoint(
          &(m_options.m_modules), &(m_options.m_filenames), std::move(regexp),
          m_options.m_language, m_options.m_skip_prologue, internal,
          m_options.m_hardware);
    } break;
    case eSetTypeSourceRegexp: // Breakpoint by regexp on source text.
    {
      const size_t num_files = m_options.m_filenames.GetSize();

      if (num_files == 0 && !m_options.m_all_files) {
        FileSpec file;
        if (!GetDefaultFile(target, file, result)) {
          result.AppendError(
              "No files provided and could not find default file.");
          result.SetStatus(eReturnStatusFailed);
          return false;
        } else {
          m_options.m_filenames.Append(file);
        }
      }

      RegularExpression regexp(m_options.m_source_text_regexp);
      if (llvm::Error err = regexp.GetError()) {
        result.AppendErrorWithFormat(
            "Source text regular expression could not be compiled: \"%s\"",
            llvm::toString(std::move(err)).c_str());
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
      bp_sp = target.CreateSourceRegexBreakpoint(
          &(m_options.m_modules), &(m_options.m_filenames),
          m_options.m_source_regex_func_names, std::move(regexp), internal,
          m_options.m_hardware, m_options.m_move_to_nearest_code);
    } break;
    case eSetTypeException: {
      Status precond_error;
      bp_sp = target.CreateExceptionBreakpoint(
          m_options.m_exception_language, m_options.m_catch_bp,
          m_options.m_throw_bp, internal, &m_options.m_exception_extra_args,
          &precond_error);
      if (precond_error.Fail()) {
        result.AppendErrorWithFormat(
            "Error setting extra exception arguments: %s",
            precond_error.AsCString());
        target.RemoveBreakpointByID(bp_sp->GetID());
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
    } break;
    case eSetTypeScripted: {

      Status error;
      bp_sp = target.CreateScriptedBreakpoint(
          m_python_class_options.GetName().c_str(), &(m_options.m_modules),
          &(m_options.m_filenames), false, m_options.m_hardware,
          m_python_class_options.GetStructuredData(), &error);
      if (error.Fail()) {
        result.AppendErrorWithFormat(
            "Error setting extra exception arguments: %s", error.AsCString());
        target.RemoveBreakpointByID(bp_sp->GetID());
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
    } break;
    default:
      break;
    }

    // Now set the various options that were passed in:
    if (bp_sp) {
      bp_sp->GetOptions().CopyOverSetOptions(m_bp_opts.GetBreakpointOptions());

      if (!m_options.m_breakpoint_names.empty()) {
        Status name_error;
        for (auto name : m_options.m_breakpoint_names) {
          target.AddNameToBreakpoint(bp_sp, name.c_str(), name_error);
          if (name_error.Fail()) {
            result.AppendErrorWithFormat("Invalid breakpoint name: %s",
                                         name.c_str());
            target.RemoveBreakpointByID(bp_sp->GetID());
            result.SetStatus(eReturnStatusFailed);
            return false;
          }
        }
      }
    }

    if (bp_sp) {
      Stream &output_stream = result.GetOutputStream();
      const bool show_locations = false;
      bp_sp->GetDescription(&output_stream, lldb::eDescriptionLevelInitial,
                            show_locations);
      if (&target == &GetDummyTarget())
        output_stream.Printf("Breakpoint set in dummy target, will get copied "
                             "into future targets.\n");
      else {
        // Don't print out this warning for exception breakpoints.  They can
        // get set before the target is set, but we won't know how to actually
        // set the breakpoint till we run.
        if (bp_sp->GetNumLocations() == 0 && break_type != eSetTypeException) {
          output_stream.Printf("WARNING:  Unable to resolve breakpoint to any "
                               "actual locations.\n");
        }
      }
      result.SetStatus(eReturnStatusSuccessFinishResult);
    } else if (!bp_sp) {
      result.AppendError("Breakpoint creation failed: No breakpoint created.");
      result.SetStatus(eReturnStatusFailed);
    }

    return result.Succeeded();
  }

private:
  bool GetDefaultFile(Target &target, FileSpec &file,
                      CommandReturnObject &result) {
    uint32_t default_line;
    // First use the Source Manager's default file. Then use the current stack
    // frame's file.
    if (!target.GetSourceManager().GetDefaultFileAndLine(file, default_line)) {
      StackFrame *cur_frame = m_exe_ctx.GetFramePtr();
      if (cur_frame == nullptr) {
        result.AppendError(
            "No selected frame to use to find the default file.");
        result.SetStatus(eReturnStatusFailed);
        return false;
      } else if (!cur_frame->HasDebugInformation()) {
        result.AppendError("Cannot use the selected frame to find the default "
                           "file, it has no debug info.");
        result.SetStatus(eReturnStatusFailed);
        return false;
      } else {
        const SymbolContext &sc =
            cur_frame->GetSymbolContext(eSymbolContextLineEntry);
        if (sc.line_entry.file) {
          file = sc.line_entry.file;
        } else {
          result.AppendError("Can't find the file for the selected frame to "
                             "use as the default file.");
          result.SetStatus(eReturnStatusFailed);
          return false;
        }
      }
    }
    return true;
  }

  BreakpointOptionGroup m_bp_opts;
  BreakpointDummyOptionGroup m_dummy_options;
  OptionGroupPythonClassWithDict m_python_class_options;
  CommandOptions m_options;
  OptionGroupOptions m_all_options;
};

// CommandObjectBreakpointModify
#pragma mark Modify

class CommandObjectBreakpointModify : public CommandObjectParsed {
public:
  CommandObjectBreakpointModify(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "breakpoint modify",
                            "Modify the options on a breakpoint or set of "
                            "breakpoints in the executable.  "
                            "If no breakpoint is specified, acts on the last "
                            "created breakpoint.  "
                            "With the exception of -e, -d and -i, passing an "
                            "empty argument clears the modification.",
                            nullptr),
        m_options() {
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeBreakpointID,
                                      eArgTypeBreakpointIDRange);
    // Add the entry for the first argument for this command to the object's
    // arguments vector.
    m_arguments.push_back(arg);

    m_options.Append(&m_bp_opts,
                     LLDB_OPT_SET_1 | LLDB_OPT_SET_2 | LLDB_OPT_SET_3,
                     LLDB_OPT_SET_ALL);
    m_options.Append(&m_dummy_opts, LLDB_OPT_SET_1, LLDB_OPT_SET_ALL);
    m_options.Finalize();
  }

  ~CommandObjectBreakpointModify() override = default;

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), CommandCompletions::eBreakpointCompletion,
        request, nullptr);
  }

  Options *GetOptions() override { return &m_options; }

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target = GetSelectedOrDummyTarget(m_dummy_opts.m_use_dummy);

    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    BreakpointIDList valid_bp_ids;

    CommandObjectMultiwordBreakpoint::VerifyBreakpointOrLocationIDs(
        command, &target, result, &valid_bp_ids,
        BreakpointName::Permissions::PermissionKinds::disablePerm);

    if (result.Succeeded()) {
      const size_t count = valid_bp_ids.GetSize();
      for (size_t i = 0; i < count; ++i) {
        BreakpointID cur_bp_id = valid_bp_ids.GetBreakpointIDAtIndex(i);

        if (cur_bp_id.GetBreakpointID() != LLDB_INVALID_BREAK_ID) {
          Breakpoint *bp =
              target.GetBreakpointByID(cur_bp_id.GetBreakpointID()).get();
          if (cur_bp_id.GetLocationID() != LLDB_INVALID_BREAK_ID) {
            BreakpointLocation *location =
                bp->FindLocationByID(cur_bp_id.GetLocationID()).get();
            if (location)
              location->GetLocationOptions().CopyOverSetOptions(
                  m_bp_opts.GetBreakpointOptions());
          } else {
            bp->GetOptions().CopyOverSetOptions(
                m_bp_opts.GetBreakpointOptions());
          }
        }
      }
    }

    return result.Succeeded();
  }

private:
  BreakpointOptionGroup m_bp_opts;
  BreakpointDummyOptionGroup m_dummy_opts;
  OptionGroupOptions m_options;
};

// CommandObjectBreakpointEnable
#pragma mark Enable

class CommandObjectBreakpointEnable : public CommandObjectParsed {
public:
  CommandObjectBreakpointEnable(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "enable",
                            "Enable the specified disabled breakpoint(s). If "
                            "no breakpoints are specified, enable all of them.",
                            nullptr) {
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeBreakpointID,
                                      eArgTypeBreakpointIDRange);
    // Add the entry for the first argument for this command to the object's
    // arguments vector.
    m_arguments.push_back(arg);
  }

  ~CommandObjectBreakpointEnable() override = default;

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), CommandCompletions::eBreakpointCompletion,
        request, nullptr);
  }

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target = GetSelectedOrDummyTarget();

    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    const BreakpointList &breakpoints = target.GetBreakpointList();

    size_t num_breakpoints = breakpoints.GetSize();

    if (num_breakpoints == 0) {
      result.AppendError("No breakpoints exist to be enabled.");
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    if (command.empty()) {
      // No breakpoint selected; enable all currently set breakpoints.
      target.EnableAllowedBreakpoints();
      result.AppendMessageWithFormat("All breakpoints enabled. (%" PRIu64
                                     " breakpoints)\n",
                                     (uint64_t)num_breakpoints);
      result.SetStatus(eReturnStatusSuccessFinishNoResult);
    } else {
      // Particular breakpoint selected; enable that breakpoint.
      BreakpointIDList valid_bp_ids;
      CommandObjectMultiwordBreakpoint::VerifyBreakpointOrLocationIDs(
          command, &target, result, &valid_bp_ids,
          BreakpointName::Permissions::PermissionKinds::disablePerm);

      if (result.Succeeded()) {
        int enable_count = 0;
        int loc_count = 0;
        const size_t count = valid_bp_ids.GetSize();
        for (size_t i = 0; i < count; ++i) {
          BreakpointID cur_bp_id = valid_bp_ids.GetBreakpointIDAtIndex(i);

          if (cur_bp_id.GetBreakpointID() != LLDB_INVALID_BREAK_ID) {
            Breakpoint *breakpoint =
                target.GetBreakpointByID(cur_bp_id.GetBreakpointID()).get();
            if (cur_bp_id.GetLocationID() != LLDB_INVALID_BREAK_ID) {
              BreakpointLocation *location =
                  breakpoint->FindLocationByID(cur_bp_id.GetLocationID()).get();
              if (location) {
                location->SetEnabled(true);
                ++loc_count;
              }
            } else {
              breakpoint->SetEnabled(true);
              ++enable_count;
            }
          }
        }
        result.AppendMessageWithFormat("%d breakpoints enabled.\n",
                                       enable_count + loc_count);
        result.SetStatus(eReturnStatusSuccessFinishNoResult);
      }
    }

    return result.Succeeded();
  }
};

// CommandObjectBreakpointDisable
#pragma mark Disable

class CommandObjectBreakpointDisable : public CommandObjectParsed {
public:
  CommandObjectBreakpointDisable(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "breakpoint disable",
            "Disable the specified breakpoint(s) without deleting "
            "them.  If none are specified, disable all "
            "breakpoints.",
            nullptr) {
    SetHelpLong(
        "Disable the specified breakpoint(s) without deleting them.  \
If none are specified, disable all breakpoints."
        R"(

)"
        "Note: disabling a breakpoint will cause none of its locations to be hit \
regardless of whether individual locations are enabled or disabled.  After the sequence:"
        R"(

    (lldb) break disable 1
    (lldb) break enable 1.1

execution will NOT stop at location 1.1.  To achieve that, type:

    (lldb) break disable 1.*
    (lldb) break enable 1.1

)"
        "The first command disables all locations for breakpoint 1, \
the second re-enables the first location.");

    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeBreakpointID,
                                      eArgTypeBreakpointIDRange);
    // Add the entry for the first argument for this command to the object's
    // arguments vector.
    m_arguments.push_back(arg);
  }

  ~CommandObjectBreakpointDisable() override = default;

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), CommandCompletions::eBreakpointCompletion,
        request, nullptr);
  }

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target = GetSelectedOrDummyTarget();
    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    const BreakpointList &breakpoints = target.GetBreakpointList();
    size_t num_breakpoints = breakpoints.GetSize();

    if (num_breakpoints == 0) {
      result.AppendError("No breakpoints exist to be disabled.");
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    if (command.empty()) {
      // No breakpoint selected; disable all currently set breakpoints.
      target.DisableAllowedBreakpoints();
      result.AppendMessageWithFormat("All breakpoints disabled. (%" PRIu64
                                     " breakpoints)\n",
                                     (uint64_t)num_breakpoints);
      result.SetStatus(eReturnStatusSuccessFinishNoResult);
    } else {
      // Particular breakpoint selected; disable that breakpoint.
      BreakpointIDList valid_bp_ids;

      CommandObjectMultiwordBreakpoint::VerifyBreakpointOrLocationIDs(
          command, &target, result, &valid_bp_ids,
          BreakpointName::Permissions::PermissionKinds::disablePerm);

      if (result.Succeeded()) {
        int disable_count = 0;
        int loc_count = 0;
        const size_t count = valid_bp_ids.GetSize();
        for (size_t i = 0; i < count; ++i) {
          BreakpointID cur_bp_id = valid_bp_ids.GetBreakpointIDAtIndex(i);

          if (cur_bp_id.GetBreakpointID() != LLDB_INVALID_BREAK_ID) {
            Breakpoint *breakpoint =
                target.GetBreakpointByID(cur_bp_id.GetBreakpointID()).get();
            if (cur_bp_id.GetLocationID() != LLDB_INVALID_BREAK_ID) {
              BreakpointLocation *location =
                  breakpoint->FindLocationByID(cur_bp_id.GetLocationID()).get();
              if (location) {
                location->SetEnabled(false);
                ++loc_count;
              }
            } else {
              breakpoint->SetEnabled(false);
              ++disable_count;
            }
          }
        }
        result.AppendMessageWithFormat("%d breakpoints disabled.\n",
                                       disable_count + loc_count);
        result.SetStatus(eReturnStatusSuccessFinishNoResult);
      }
    }

    return result.Succeeded();
  }
};

// CommandObjectBreakpointList

#pragma mark List::CommandOptions
#define LLDB_OPTIONS_breakpoint_list
#include "CommandOptions.inc"

#pragma mark List

class CommandObjectBreakpointList : public CommandObjectParsed {
public:
  CommandObjectBreakpointList(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "breakpoint list",
            "List some or all breakpoints at configurable levels of detail.",
            nullptr),
        m_options() {
    CommandArgumentEntry arg;
    CommandArgumentData bp_id_arg;

    // Define the first (and only) variant of this arg.
    bp_id_arg.arg_type = eArgTypeBreakpointID;
    bp_id_arg.arg_repetition = eArgRepeatOptional;

    // There is only one variant this argument could be; put it into the
    // argument entry.
    arg.push_back(bp_id_arg);

    // Push the data for the first argument into the m_arguments vector.
    m_arguments.push_back(arg);
  }

  ~CommandObjectBreakpointList() override = default;

  Options *GetOptions() override { return &m_options; }

  class CommandOptions : public Options {
  public:
    CommandOptions() : Options() {}

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;

      switch (short_option) {
      case 'b':
        m_level = lldb::eDescriptionLevelBrief;
        break;
      case 'D':
        m_use_dummy = true;
        break;
      case 'f':
        m_level = lldb::eDescriptionLevelFull;
        break;
      case 'v':
        m_level = lldb::eDescriptionLevelVerbose;
        break;
      case 'i':
        m_internal = true;
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_level = lldb::eDescriptionLevelFull;
      m_internal = false;
      m_use_dummy = false;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::makeArrayRef(g_breakpoint_list_options);
    }

    // Instance variables to hold the values for command options.

    lldb::DescriptionLevel m_level = lldb::eDescriptionLevelBrief;

    bool m_internal;
    bool m_use_dummy = false;
  };

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target = GetSelectedOrDummyTarget(m_options.m_use_dummy);

    const BreakpointList &breakpoints =
        target.GetBreakpointList(m_options.m_internal);
    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList(m_options.m_internal).GetListMutex(lock);

    size_t num_breakpoints = breakpoints.GetSize();

    if (num_breakpoints == 0) {
      result.AppendMessage("No breakpoints currently set.");
      result.SetStatus(eReturnStatusSuccessFinishNoResult);
      return true;
    }

    Stream &output_stream = result.GetOutputStream();

    if (command.empty()) {
      // No breakpoint selected; show info about all currently set breakpoints.
      result.AppendMessage("Current breakpoints:");
      for (size_t i = 0; i < num_breakpoints; ++i) {
        Breakpoint *breakpoint = breakpoints.GetBreakpointAtIndex(i).get();
        if (breakpoint->AllowList())
          AddBreakpointDescription(&output_stream, breakpoint,
                                   m_options.m_level);
      }
      result.SetStatus(eReturnStatusSuccessFinishNoResult);
    } else {
      // Particular breakpoints selected; show info about that breakpoint.
      BreakpointIDList valid_bp_ids;
      CommandObjectMultiwordBreakpoint::VerifyBreakpointOrLocationIDs(
          command, &target, result, &valid_bp_ids,
          BreakpointName::Permissions::PermissionKinds::listPerm);

      if (result.Succeeded()) {
        for (size_t i = 0; i < valid_bp_ids.GetSize(); ++i) {
          BreakpointID cur_bp_id = valid_bp_ids.GetBreakpointIDAtIndex(i);
          Breakpoint *breakpoint =
              target.GetBreakpointByID(cur_bp_id.GetBreakpointID()).get();
          AddBreakpointDescription(&output_stream, breakpoint,
                                   m_options.m_level);
        }
        result.SetStatus(eReturnStatusSuccessFinishNoResult);
      } else {
        result.AppendError("Invalid breakpoint ID.");
        result.SetStatus(eReturnStatusFailed);
      }
    }

    return result.Succeeded();
  }

private:
  CommandOptions m_options;
};

// CommandObjectBreakpointClear
#pragma mark Clear::CommandOptions

#define LLDB_OPTIONS_breakpoint_clear
#include "CommandOptions.inc"

#pragma mark Clear

class CommandObjectBreakpointClear : public CommandObjectParsed {
public:
  enum BreakpointClearType { eClearTypeInvalid, eClearTypeFileAndLine };

  CommandObjectBreakpointClear(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "breakpoint clear",
                            "Delete or disable breakpoints matching the "
                            "specified source file and line.",
                            "breakpoint clear <cmd-options>"),
        m_options() {}

  ~CommandObjectBreakpointClear() override = default;

  Options *GetOptions() override { return &m_options; }

  class CommandOptions : public Options {
  public:
    CommandOptions() : Options(), m_filename() {}

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;

      switch (short_option) {
      case 'f':
        m_filename.assign(std::string(option_arg));
        break;

      case 'l':
        option_arg.getAsInteger(0, m_line_num);
        break;

      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_filename.clear();
      m_line_num = 0;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::makeArrayRef(g_breakpoint_clear_options);
    }

    // Instance variables to hold the values for command options.

    std::string m_filename;
    uint32_t m_line_num = 0;
  };

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target = GetSelectedOrDummyTarget();

    // The following are the various types of breakpoints that could be
    // cleared:
    //   1). -f -l (clearing breakpoint by source location)

    BreakpointClearType break_type = eClearTypeInvalid;

    if (m_options.m_line_num != 0)
      break_type = eClearTypeFileAndLine;

    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    BreakpointList &breakpoints = target.GetBreakpointList();
    size_t num_breakpoints = breakpoints.GetSize();

    // Early return if there's no breakpoint at all.
    if (num_breakpoints == 0) {
      result.AppendError("Breakpoint clear: No breakpoint cleared.");
      result.SetStatus(eReturnStatusFailed);
      return result.Succeeded();
    }

    // Find matching breakpoints and delete them.

    // First create a copy of all the IDs.
    std::vector<break_id_t> BreakIDs;
    for (size_t i = 0; i < num_breakpoints; ++i)
      BreakIDs.push_back(breakpoints.GetBreakpointAtIndex(i)->GetID());

    int num_cleared = 0;
    StreamString ss;
    switch (break_type) {
    case eClearTypeFileAndLine: // Breakpoint by source position
    {
      const ConstString filename(m_options.m_filename.c_str());
      BreakpointLocationCollection loc_coll;

      for (size_t i = 0; i < num_breakpoints; ++i) {
        Breakpoint *bp = breakpoints.FindBreakpointByID(BreakIDs[i]).get();

        if (bp->GetMatchingFileLine(filename, m_options.m_line_num, loc_coll)) {
          // If the collection size is 0, it's a full match and we can just
          // remove the breakpoint.
          if (loc_coll.GetSize() == 0) {
            bp->GetDescription(&ss, lldb::eDescriptionLevelBrief);
            ss.EOL();
            target.RemoveBreakpointByID(bp->GetID());
            ++num_cleared;
          }
        }
      }
    } break;

    default:
      break;
    }

    if (num_cleared > 0) {
      Stream &output_stream = result.GetOutputStream();
      output_stream.Printf("%d breakpoints cleared:\n", num_cleared);
      output_stream << ss.GetString();
      output_stream.EOL();
      result.SetStatus(eReturnStatusSuccessFinishNoResult);
    } else {
      result.AppendError("Breakpoint clear: No breakpoint cleared.");
      result.SetStatus(eReturnStatusFailed);
    }

    return result.Succeeded();
  }

private:
  CommandOptions m_options;
};

// CommandObjectBreakpointDelete
#define LLDB_OPTIONS_breakpoint_delete
#include "CommandOptions.inc"

#pragma mark Delete

class CommandObjectBreakpointDelete : public CommandObjectParsed {
public:
  CommandObjectBreakpointDelete(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "breakpoint delete",
                            "Delete the specified breakpoint(s).  If no "
                            "breakpoints are specified, delete them all.",
                            nullptr),
        m_options() {
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeBreakpointID,
                                      eArgTypeBreakpointIDRange);
    // Add the entry for the first argument for this command to the object's
    // arguments vector.
    m_arguments.push_back(arg);
  }

  ~CommandObjectBreakpointDelete() override = default;

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), CommandCompletions::eBreakpointCompletion,
        request, nullptr);
  }

  Options *GetOptions() override { return &m_options; }

  class CommandOptions : public Options {
  public:
    CommandOptions() : Options() {}

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;

      switch (short_option) {
      case 'f':
        m_force = true;
        break;

      case 'D':
        m_use_dummy = true;
        break;
        
      case 'd':
        m_delete_disabled = true;
        break;

      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_use_dummy = false;
      m_force = false;
      m_delete_disabled = false;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::makeArrayRef(g_breakpoint_delete_options);
    }

    // Instance variables to hold the values for command options.
    bool m_use_dummy = false;
    bool m_force = false;
    bool m_delete_disabled = false;
  };

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target = GetSelectedOrDummyTarget(m_options.m_use_dummy);
    result.Clear();
    
    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    BreakpointList &breakpoints = target.GetBreakpointList();

    size_t num_breakpoints = breakpoints.GetSize();

    if (num_breakpoints == 0) {
      result.AppendError("No breakpoints exist to be deleted.");
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    if (command.empty() && !m_options.m_delete_disabled) {
      if (!m_options.m_force &&
          !m_interpreter.Confirm(
              "About to delete all breakpoints, do you want to do that?",
              true)) {
        result.AppendMessage("Operation cancelled...");
      } else {
        target.RemoveAllowedBreakpoints();
        result.AppendMessageWithFormat(
            "All breakpoints removed. (%" PRIu64 " breakpoint%s)\n",
            (uint64_t)num_breakpoints, num_breakpoints > 1 ? "s" : "");
      }
      result.SetStatus(eReturnStatusSuccessFinishNoResult);
    } else {
      // Particular breakpoint selected; disable that breakpoint.
      BreakpointIDList valid_bp_ids;
      
      if (m_options.m_delete_disabled) {
        BreakpointIDList excluded_bp_ids;

        if (!command.empty()) {
          CommandObjectMultiwordBreakpoint::VerifyBreakpointOrLocationIDs(
              command, &target, result, &excluded_bp_ids,
              BreakpointName::Permissions::PermissionKinds::deletePerm);
        }
        for (auto breakpoint_sp : breakpoints.Breakpoints()) {
          if (!breakpoint_sp->IsEnabled() && breakpoint_sp->AllowDelete()) {
            BreakpointID bp_id(breakpoint_sp->GetID());
            size_t pos = 0;
            if (!excluded_bp_ids.FindBreakpointID(bp_id, &pos))
              valid_bp_ids.AddBreakpointID(breakpoint_sp->GetID());
          }
        }
        if (valid_bp_ids.GetSize() == 0) {
          result.AppendError("No disabled breakpoints.");
          result.SetStatus(eReturnStatusFailed);
          return false;
        }
      } else {
        CommandObjectMultiwordBreakpoint::VerifyBreakpointOrLocationIDs(
            command, &target, result, &valid_bp_ids,
            BreakpointName::Permissions::PermissionKinds::deletePerm);
      }
      
      if (result.Succeeded()) {
        int delete_count = 0;
        int disable_count = 0;
        const size_t count = valid_bp_ids.GetSize();
        for (size_t i = 0; i < count; ++i) {
          BreakpointID cur_bp_id = valid_bp_ids.GetBreakpointIDAtIndex(i);

          if (cur_bp_id.GetBreakpointID() != LLDB_INVALID_BREAK_ID) {
            if (cur_bp_id.GetLocationID() != LLDB_INVALID_BREAK_ID) {
              Breakpoint *breakpoint =
                  target.GetBreakpointByID(cur_bp_id.GetBreakpointID()).get();
              BreakpointLocation *location =
                  breakpoint->FindLocationByID(cur_bp_id.GetLocationID()).get();
              // It makes no sense to try to delete individual locations, so we
              // disable them instead.
              if (location) {
                location->SetEnabled(false);
                ++disable_count;
              }
            } else {
              target.RemoveBreakpointByID(cur_bp_id.GetBreakpointID());
              ++delete_count;
            }
          }
        }
        result.AppendMessageWithFormat(
            "%d breakpoints deleted; %d breakpoint locations disabled.\n",
            delete_count, disable_count);
        result.SetStatus(eReturnStatusSuccessFinishNoResult);
      }
    }
    return result.Succeeded();
  }

private:
  CommandOptions m_options;
};

// CommandObjectBreakpointName
#define LLDB_OPTIONS_breakpoint_name
#include "CommandOptions.inc"

class BreakpointNameOptionGroup : public OptionGroup {
public:
  BreakpointNameOptionGroup()
      : OptionGroup(), m_breakpoint(LLDB_INVALID_BREAK_ID), m_use_dummy(false) {
  }

  ~BreakpointNameOptionGroup() override = default;

  llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
    return llvm::makeArrayRef(g_breakpoint_name_options);
  }

  Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                        ExecutionContext *execution_context) override {
    Status error;
    const int short_option = g_breakpoint_name_options[option_idx].short_option;

    switch (short_option) {
    case 'N':
      if (BreakpointID::StringIsBreakpointName(option_arg, error) &&
          error.Success())
        m_name.SetValueFromString(option_arg);
      break;
    case 'B':
      if (m_breakpoint.SetValueFromString(option_arg).Fail())
        error.SetErrorStringWithFormat(
            "unrecognized value \"%s\" for breakpoint",
            option_arg.str().c_str());
      break;
    case 'D':
      if (m_use_dummy.SetValueFromString(option_arg).Fail())
        error.SetErrorStringWithFormat(
            "unrecognized value \"%s\" for use-dummy",
            option_arg.str().c_str());
      break;
    case 'H':
      m_help_string.SetValueFromString(option_arg);
      break;

    default:
      llvm_unreachable("Unimplemented option");
    }
    return error;
  }

  void OptionParsingStarting(ExecutionContext *execution_context) override {
    m_name.Clear();
    m_breakpoint.Clear();
    m_use_dummy.Clear();
    m_use_dummy.SetDefaultValue(false);
    m_help_string.Clear();
  }

  OptionValueString m_name;
  OptionValueUInt64 m_breakpoint;
  OptionValueBoolean m_use_dummy;
  OptionValueString m_help_string;
};

#define LLDB_OPTIONS_breakpoint_access
#include "CommandOptions.inc"

class BreakpointAccessOptionGroup : public OptionGroup {
public:
  BreakpointAccessOptionGroup() : OptionGroup() {}

  ~BreakpointAccessOptionGroup() override = default;

  llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
    return llvm::makeArrayRef(g_breakpoint_access_options);
  }
  Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                        ExecutionContext *execution_context) override {
    Status error;
    const int short_option =
        g_breakpoint_access_options[option_idx].short_option;

    switch (short_option) {
    case 'L': {
      bool value, success;
      value = OptionArgParser::ToBoolean(option_arg, false, &success);
      if (success) {
        m_permissions.SetAllowList(value);
      } else
        error.SetErrorStringWithFormat(
            "invalid boolean value '%s' passed for -L option",
            option_arg.str().c_str());
    } break;
    case 'A': {
      bool value, success;
      value = OptionArgParser::ToBoolean(option_arg, false, &success);
      if (success) {
        m_permissions.SetAllowDisable(value);
      } else
        error.SetErrorStringWithFormat(
            "invalid boolean value '%s' passed for -L option",
            option_arg.str().c_str());
    } break;
    case 'D': {
      bool value, success;
      value = OptionArgParser::ToBoolean(option_arg, false, &success);
      if (success) {
        m_permissions.SetAllowDelete(value);
      } else
        error.SetErrorStringWithFormat(
            "invalid boolean value '%s' passed for -L option",
            option_arg.str().c_str());
    } break;
    default:
      llvm_unreachable("Unimplemented option");
    }

    return error;
  }

  void OptionParsingStarting(ExecutionContext *execution_context) override {}

  const BreakpointName::Permissions &GetPermissions() const {
    return m_permissions;
  }
  BreakpointName::Permissions m_permissions;
};

class CommandObjectBreakpointNameConfigure : public CommandObjectParsed {
public:
  CommandObjectBreakpointNameConfigure(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "configure",
            "Configure the options for the breakpoint"
            " name provided.  "
            "If you provide a breakpoint id, the options will be copied from "
            "the breakpoint, otherwise only the options specified will be set "
            "on the name.",
            "breakpoint name configure <command-options> "
            "<breakpoint-name-list>"),
        m_bp_opts(), m_option_group() {
    // Create the first variant for the first (and only) argument for this
    // command.
    CommandArgumentEntry arg1;
    CommandArgumentData id_arg;
    id_arg.arg_type = eArgTypeBreakpointName;
    id_arg.arg_repetition = eArgRepeatOptional;
    arg1.push_back(id_arg);
    m_arguments.push_back(arg1);

    m_option_group.Append(&m_bp_opts, LLDB_OPT_SET_ALL, LLDB_OPT_SET_1);
    m_option_group.Append(&m_access_options, LLDB_OPT_SET_ALL,
                          LLDB_OPT_SET_ALL);
    m_option_group.Append(&m_bp_id, LLDB_OPT_SET_2 | LLDB_OPT_SET_4,
                          LLDB_OPT_SET_ALL);
    m_option_group.Finalize();
  }

  ~CommandObjectBreakpointNameConfigure() override = default;

  Options *GetOptions() override { return &m_option_group; }

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {

    const size_t argc = command.GetArgumentCount();
    if (argc == 0) {
      result.AppendError("No names provided.");
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    Target &target = GetSelectedOrDummyTarget(false);

    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    // Make a pass through first to see that all the names are legal.
    for (auto &entry : command.entries()) {
      Status error;
      if (!BreakpointID::StringIsBreakpointName(entry.ref(), error)) {
        result.AppendErrorWithFormat("Invalid breakpoint name: %s - %s",
                                     entry.c_str(), error.AsCString());
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
    }
    // Now configure them, we already pre-checked the names so we don't need to
    // check the error:
    BreakpointSP bp_sp;
    if (m_bp_id.m_breakpoint.OptionWasSet()) {
      lldb::break_id_t bp_id = m_bp_id.m_breakpoint.GetUInt64Value();
      bp_sp = target.GetBreakpointByID(bp_id);
      if (!bp_sp) {
        result.AppendErrorWithFormatv("Could not find specified breakpoint {0}",
                                      bp_id);
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
    }

    Status error;
    for (auto &entry : command.entries()) {
      ConstString name(entry.c_str());
      BreakpointName *bp_name = target.FindBreakpointName(name, true, error);
      if (!bp_name)
        continue;
      if (m_bp_id.m_help_string.OptionWasSet())
        bp_name->SetHelp(m_bp_id.m_help_string.GetStringValue().str().c_str());

      if (bp_sp)
        target.ConfigureBreakpointName(*bp_name, bp_sp->GetOptions(),
                                       m_access_options.GetPermissions());
      else
        target.ConfigureBreakpointName(*bp_name,
                                       m_bp_opts.GetBreakpointOptions(),
                                       m_access_options.GetPermissions());
    }
    return true;
  }

private:
  BreakpointNameOptionGroup m_bp_id; // Only using the id part of this.
  BreakpointOptionGroup m_bp_opts;
  BreakpointAccessOptionGroup m_access_options;
  OptionGroupOptions m_option_group;
};

class CommandObjectBreakpointNameAdd : public CommandObjectParsed {
public:
  CommandObjectBreakpointNameAdd(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "add", "Add a name to the breakpoints provided.",
            "breakpoint name add <command-options> <breakpoint-id-list>"),
        m_name_options(), m_option_group() {
    // Create the first variant for the first (and only) argument for this
    // command.
    CommandArgumentEntry arg1;
    CommandArgumentData id_arg;
    id_arg.arg_type = eArgTypeBreakpointID;
    id_arg.arg_repetition = eArgRepeatOptional;
    arg1.push_back(id_arg);
    m_arguments.push_back(arg1);

    m_option_group.Append(&m_name_options, LLDB_OPT_SET_1, LLDB_OPT_SET_ALL);
    m_option_group.Finalize();
  }

  ~CommandObjectBreakpointNameAdd() override = default;

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), CommandCompletions::eBreakpointCompletion,
        request, nullptr);
  }

  Options *GetOptions() override { return &m_option_group; }

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    if (!m_name_options.m_name.OptionWasSet()) {
      result.SetError("No name option provided.");
      return false;
    }

    Target &target =
        GetSelectedOrDummyTarget(m_name_options.m_use_dummy.GetCurrentValue());

    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    const BreakpointList &breakpoints = target.GetBreakpointList();

    size_t num_breakpoints = breakpoints.GetSize();
    if (num_breakpoints == 0) {
      result.SetError("No breakpoints, cannot add names.");
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    // Particular breakpoint selected; disable that breakpoint.
    BreakpointIDList valid_bp_ids;
    CommandObjectMultiwordBreakpoint::VerifyBreakpointIDs(
        command, &target, result, &valid_bp_ids,
        BreakpointName::Permissions::PermissionKinds::listPerm);

    if (result.Succeeded()) {
      if (valid_bp_ids.GetSize() == 0) {
        result.SetError("No breakpoints specified, cannot add names.");
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
      size_t num_valid_ids = valid_bp_ids.GetSize();
      const char *bp_name = m_name_options.m_name.GetCurrentValue();
      Status error; // This error reports illegal names, but we've already
                    // checked that, so we don't need to check it again here.
      for (size_t index = 0; index < num_valid_ids; index++) {
        lldb::break_id_t bp_id =
            valid_bp_ids.GetBreakpointIDAtIndex(index).GetBreakpointID();
        BreakpointSP bp_sp = breakpoints.FindBreakpointByID(bp_id);
        target.AddNameToBreakpoint(bp_sp, bp_name, error);
      }
    }

    return true;
  }

private:
  BreakpointNameOptionGroup m_name_options;
  OptionGroupOptions m_option_group;
};

class CommandObjectBreakpointNameDelete : public CommandObjectParsed {
public:
  CommandObjectBreakpointNameDelete(CommandInterpreter &interpreter)
      : CommandObjectParsed(
            interpreter, "delete",
            "Delete a name from the breakpoints provided.",
            "breakpoint name delete <command-options> <breakpoint-id-list>"),
        m_name_options(), m_option_group() {
    // Create the first variant for the first (and only) argument for this
    // command.
    CommandArgumentEntry arg1;
    CommandArgumentData id_arg;
    id_arg.arg_type = eArgTypeBreakpointID;
    id_arg.arg_repetition = eArgRepeatOptional;
    arg1.push_back(id_arg);
    m_arguments.push_back(arg1);

    m_option_group.Append(&m_name_options, LLDB_OPT_SET_1, LLDB_OPT_SET_ALL);
    m_option_group.Finalize();
  }

  ~CommandObjectBreakpointNameDelete() override = default;

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), CommandCompletions::eBreakpointCompletion,
        request, nullptr);
  }

  Options *GetOptions() override { return &m_option_group; }

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    if (!m_name_options.m_name.OptionWasSet()) {
      result.SetError("No name option provided.");
      return false;
    }

    Target &target =
        GetSelectedOrDummyTarget(m_name_options.m_use_dummy.GetCurrentValue());

    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    const BreakpointList &breakpoints = target.GetBreakpointList();

    size_t num_breakpoints = breakpoints.GetSize();
    if (num_breakpoints == 0) {
      result.SetError("No breakpoints, cannot delete names.");
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    // Particular breakpoint selected; disable that breakpoint.
    BreakpointIDList valid_bp_ids;
    CommandObjectMultiwordBreakpoint::VerifyBreakpointIDs(
        command, &target, result, &valid_bp_ids,
        BreakpointName::Permissions::PermissionKinds::deletePerm);

    if (result.Succeeded()) {
      if (valid_bp_ids.GetSize() == 0) {
        result.SetError("No breakpoints specified, cannot delete names.");
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
      ConstString bp_name(m_name_options.m_name.GetCurrentValue());
      size_t num_valid_ids = valid_bp_ids.GetSize();
      for (size_t index = 0; index < num_valid_ids; index++) {
        lldb::break_id_t bp_id =
            valid_bp_ids.GetBreakpointIDAtIndex(index).GetBreakpointID();
        BreakpointSP bp_sp = breakpoints.FindBreakpointByID(bp_id);
        target.RemoveNameFromBreakpoint(bp_sp, bp_name);
      }
    }

    return true;
  }

private:
  BreakpointNameOptionGroup m_name_options;
  OptionGroupOptions m_option_group;
};

class CommandObjectBreakpointNameList : public CommandObjectParsed {
public:
  CommandObjectBreakpointNameList(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "list",
                            "List either the names for a breakpoint or info "
                            "about a given name.  With no arguments, lists all "
                            "names",
                            "breakpoint name list <command-options>"),
        m_name_options(), m_option_group() {
    m_option_group.Append(&m_name_options, LLDB_OPT_SET_3, LLDB_OPT_SET_ALL);
    m_option_group.Finalize();
  }

  ~CommandObjectBreakpointNameList() override = default;

  Options *GetOptions() override { return &m_option_group; }

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target =
        GetSelectedOrDummyTarget(m_name_options.m_use_dummy.GetCurrentValue());

    std::vector<std::string> name_list;
    if (command.empty()) {
      target.GetBreakpointNames(name_list);
    } else {
      for (const Args::ArgEntry &arg : command) {
        name_list.push_back(arg.c_str());
      }
    }

    if (name_list.empty()) {
      result.AppendMessage("No breakpoint names found.");
    } else {
      for (const std::string &name_str : name_list) {
        const char *name = name_str.c_str();
        // First print out the options for the name:
        Status error;
        BreakpointName *bp_name =
            target.FindBreakpointName(ConstString(name), false, error);
        if (bp_name) {
          StreamString s;
          result.AppendMessageWithFormat("Name: %s\n", name);
          if (bp_name->GetDescription(&s, eDescriptionLevelFull)) {
            result.AppendMessage(s.GetString());
          }

          std::unique_lock<std::recursive_mutex> lock;
          target.GetBreakpointList().GetListMutex(lock);

          BreakpointList &breakpoints = target.GetBreakpointList();
          bool any_set = false;
          for (BreakpointSP bp_sp : breakpoints.Breakpoints()) {
            if (bp_sp->MatchesName(name)) {
              StreamString s;
              any_set = true;
              bp_sp->GetDescription(&s, eDescriptionLevelBrief);
              s.EOL();
              result.AppendMessage(s.GetString());
            }
          }
          if (!any_set)
            result.AppendMessage("No breakpoints using this name.");
        } else {
          result.AppendMessageWithFormat("Name: %s not found.\n", name);
        }
      }
    }
    return true;
  }

private:
  BreakpointNameOptionGroup m_name_options;
  OptionGroupOptions m_option_group;
};

// CommandObjectBreakpointName
class CommandObjectBreakpointName : public CommandObjectMultiword {
public:
  CommandObjectBreakpointName(CommandInterpreter &interpreter)
      : CommandObjectMultiword(
            interpreter, "name", "Commands to manage name tags for breakpoints",
            "breakpoint name <subcommand> [<command-options>]") {
    CommandObjectSP add_command_object(
        new CommandObjectBreakpointNameAdd(interpreter));
    CommandObjectSP delete_command_object(
        new CommandObjectBreakpointNameDelete(interpreter));
    CommandObjectSP list_command_object(
        new CommandObjectBreakpointNameList(interpreter));
    CommandObjectSP configure_command_object(
        new CommandObjectBreakpointNameConfigure(interpreter));

    LoadSubCommand("add", add_command_object);
    LoadSubCommand("delete", delete_command_object);
    LoadSubCommand("list", list_command_object);
    LoadSubCommand("configure", configure_command_object);
  }

  ~CommandObjectBreakpointName() override = default;
};

// CommandObjectBreakpointRead
#pragma mark Read::CommandOptions
#define LLDB_OPTIONS_breakpoint_read
#include "CommandOptions.inc"

#pragma mark Read

class CommandObjectBreakpointRead : public CommandObjectParsed {
public:
  CommandObjectBreakpointRead(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "breakpoint read",
                            "Read and set the breakpoints previously saved to "
                            "a file with \"breakpoint write\".  ",
                            nullptr),
        m_options() {}

  ~CommandObjectBreakpointRead() override = default;

  Options *GetOptions() override { return &m_options; }

  class CommandOptions : public Options {
  public:
    CommandOptions() : Options() {}

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;

      switch (short_option) {
      case 'f':
        m_filename.assign(std::string(option_arg));
        break;
      case 'N': {
        Status name_error;
        if (!BreakpointID::StringIsBreakpointName(llvm::StringRef(option_arg),
                                                  name_error)) {
          error.SetErrorStringWithFormat("Invalid breakpoint name: %s",
                                         name_error.AsCString());
        }
        m_names.push_back(std::string(option_arg));
        break;
      }
      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_filename.clear();
      m_names.clear();
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::makeArrayRef(g_breakpoint_read_options);
    }

    void HandleOptionArgumentCompletion(
        CompletionRequest &request, OptionElementVector &opt_element_vector,
        int opt_element_index, CommandInterpreter &interpreter) override {
      int opt_arg_pos = opt_element_vector[opt_element_index].opt_arg_pos;
      int opt_defs_index = opt_element_vector[opt_element_index].opt_defs_index;

      switch (GetDefinitions()[opt_defs_index].short_option) {
      case 'f':
        CommandCompletions::InvokeCommonCompletionCallbacks(
            interpreter, CommandCompletions::eDiskFileCompletion, request,
            nullptr);
        break;

      case 'N':
        llvm::Optional<FileSpec> file_spec;
        const llvm::StringRef dash_f("-f");
        for (int arg_idx = 0; arg_idx < opt_arg_pos; arg_idx++) {
          if (dash_f == request.GetParsedLine().GetArgumentAtIndex(arg_idx)) {
            file_spec.emplace(
                request.GetParsedLine().GetArgumentAtIndex(arg_idx + 1));
            break;
          }
        }
        if (!file_spec)
          return;

        FileSystem::Instance().Resolve(*file_spec);
        Status error;
        StructuredData::ObjectSP input_data_sp =
            StructuredData::ParseJSONFromFile(*file_spec, error);
        if (!error.Success())
          return;

        StructuredData::Array *bkpt_array = input_data_sp->GetAsArray();
        if (!bkpt_array)
          return;

        const size_t num_bkpts = bkpt_array->GetSize();
        for (size_t i = 0; i < num_bkpts; i++) {
          StructuredData::ObjectSP bkpt_object_sp =
              bkpt_array->GetItemAtIndex(i);
          if (!bkpt_object_sp)
            return;

          StructuredData::Dictionary *bkpt_dict =
              bkpt_object_sp->GetAsDictionary();
          if (!bkpt_dict)
            return;

          StructuredData::ObjectSP bkpt_data_sp =
              bkpt_dict->GetValueForKey(Breakpoint::GetSerializationKey());
          if (!bkpt_data_sp)
            return;

          bkpt_dict = bkpt_data_sp->GetAsDictionary();
          if (!bkpt_dict)
            return;

          StructuredData::Array *names_array;

          if (!bkpt_dict->GetValueForKeyAsArray("Names", names_array))
            return;

          size_t num_names = names_array->GetSize();

          for (size_t i = 0; i < num_names; i++) {
            llvm::StringRef name;
            if (names_array->GetItemAtIndexAsString(i, name))
              request.TryCompleteCurrentArg(name);
          }
        }
      }
    }

    std::string m_filename;
    std::vector<std::string> m_names;
  };

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target = GetSelectedOrDummyTarget();

    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    FileSpec input_spec(m_options.m_filename);
    FileSystem::Instance().Resolve(input_spec);
    BreakpointIDList new_bps;
    Status error = target.CreateBreakpointsFromFile(input_spec,
                                                    m_options.m_names, new_bps);

    if (!error.Success()) {
      result.AppendError(error.AsCString());
      result.SetStatus(eReturnStatusFailed);
      return false;
    }

    Stream &output_stream = result.GetOutputStream();

    size_t num_breakpoints = new_bps.GetSize();
    if (num_breakpoints == 0) {
      result.AppendMessage("No breakpoints added.");
    } else {
      // No breakpoint selected; show info about all currently set breakpoints.
      result.AppendMessage("New breakpoints:");
      for (size_t i = 0; i < num_breakpoints; ++i) {
        BreakpointID bp_id = new_bps.GetBreakpointIDAtIndex(i);
        Breakpoint *bp = target.GetBreakpointList()
                             .FindBreakpointByID(bp_id.GetBreakpointID())
                             .get();
        if (bp)
          bp->GetDescription(&output_stream, lldb::eDescriptionLevelInitial,
                             false);
      }
    }
    return result.Succeeded();
  }

private:
  CommandOptions m_options;
};

// CommandObjectBreakpointWrite
#pragma mark Write::CommandOptions
#define LLDB_OPTIONS_breakpoint_write
#include "CommandOptions.inc"

#pragma mark Write
class CommandObjectBreakpointWrite : public CommandObjectParsed {
public:
  CommandObjectBreakpointWrite(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "breakpoint write",
                            "Write the breakpoints listed to a file that can "
                            "be read in with \"breakpoint read\".  "
                            "If given no arguments, writes all breakpoints.",
                            nullptr),
        m_options() {
    CommandArgumentEntry arg;
    CommandObject::AddIDsArgumentData(arg, eArgTypeBreakpointID,
                                      eArgTypeBreakpointIDRange);
    // Add the entry for the first argument for this command to the object's
    // arguments vector.
    m_arguments.push_back(arg);
  }

  ~CommandObjectBreakpointWrite() override = default;

  void
  HandleArgumentCompletion(CompletionRequest &request,
                           OptionElementVector &opt_element_vector) override {
    CommandCompletions::InvokeCommonCompletionCallbacks(
        GetCommandInterpreter(), CommandCompletions::eBreakpointCompletion,
        request, nullptr);
  }

  Options *GetOptions() override { return &m_options; }

  class CommandOptions : public Options {
  public:
    CommandOptions() : Options() {}

    ~CommandOptions() override = default;

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                          ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;

      switch (short_option) {
      case 'f':
        m_filename.assign(std::string(option_arg));
        break;
      case 'a':
        m_append = true;
        break;
      default:
        llvm_unreachable("Unimplemented option");
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_filename.clear();
      m_append = false;
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::makeArrayRef(g_breakpoint_write_options);
    }

    // Instance variables to hold the values for command options.

    std::string m_filename;
    bool m_append = false;
  };

protected:
  bool DoExecute(Args &command, CommandReturnObject &result) override {
    Target &target = GetSelectedOrDummyTarget();

    std::unique_lock<std::recursive_mutex> lock;
    target.GetBreakpointList().GetListMutex(lock);

    BreakpointIDList valid_bp_ids;
    if (!command.empty()) {
      CommandObjectMultiwordBreakpoint::VerifyBreakpointIDs(
          command, &target, result, &valid_bp_ids,
          BreakpointName::Permissions::PermissionKinds::listPerm);

      if (!result.Succeeded()) {
        result.SetStatus(eReturnStatusFailed);
        return false;
      }
    }
    FileSpec file_spec(m_options.m_filename);
    FileSystem::Instance().Resolve(file_spec);
    Status error = target.SerializeBreakpointsToFile(file_spec, valid_bp_ids,
                                                     m_options.m_append);
    if (!error.Success()) {
      result.AppendErrorWithFormat("error serializing breakpoints: %s.",
                                   error.AsCString());
      result.SetStatus(eReturnStatusFailed);
    }
    return result.Succeeded();
  }

private:
  CommandOptions m_options;
};

// CommandObjectMultiwordBreakpoint
#pragma mark MultiwordBreakpoint

CommandObjectMultiwordBreakpoint::CommandObjectMultiwordBreakpoint(
    CommandInterpreter &interpreter)
    : CommandObjectMultiword(
          interpreter, "breakpoint",
          "Commands for operating on breakpoints (see 'help b' for shorthand.)",
          "breakpoint <subcommand> [<command-options>]") {
  CommandObjectSP list_command_object(
      new CommandObjectBreakpointList(interpreter));
  CommandObjectSP enable_command_object(
      new CommandObjectBreakpointEnable(interpreter));
  CommandObjectSP disable_command_object(
      new CommandObjectBreakpointDisable(interpreter));
  CommandObjectSP clear_command_object(
      new CommandObjectBreakpointClear(interpreter));
  CommandObjectSP delete_command_object(
      new CommandObjectBreakpointDelete(interpreter));
  CommandObjectSP set_command_object(
      new CommandObjectBreakpointSet(interpreter));
  CommandObjectSP command_command_object(
      new CommandObjectBreakpointCommand(interpreter));
  CommandObjectSP modify_command_object(
      new CommandObjectBreakpointModify(interpreter));
  CommandObjectSP name_command_object(
      new CommandObjectBreakpointName(interpreter));
  CommandObjectSP write_command_object(
      new CommandObjectBreakpointWrite(interpreter));
  CommandObjectSP read_command_object(
      new CommandObjectBreakpointRead(interpreter));

  list_command_object->SetCommandName("breakpoint list");
  enable_command_object->SetCommandName("breakpoint enable");
  disable_command_object->SetCommandName("breakpoint disable");
  clear_command_object->SetCommandName("breakpoint clear");
  delete_command_object->SetCommandName("breakpoint delete");
  set_command_object->SetCommandName("breakpoint set");
  command_command_object->SetCommandName("breakpoint command");
  modify_command_object->SetCommandName("breakpoint modify");
  name_command_object->SetCommandName("breakpoint name");
  write_command_object->SetCommandName("breakpoint write");
  read_command_object->SetCommandName("breakpoint read");

  LoadSubCommand("list", list_command_object);
  LoadSubCommand("enable", enable_command_object);
  LoadSubCommand("disable", disable_command_object);
  LoadSubCommand("clear", clear_command_object);
  LoadSubCommand("delete", delete_command_object);
  LoadSubCommand("set", set_command_object);
  LoadSubCommand("command", command_command_object);
  LoadSubCommand("modify", modify_command_object);
  LoadSubCommand("name", name_command_object);
  LoadSubCommand("write", write_command_object);
  LoadSubCommand("read", read_command_object);
}

CommandObjectMultiwordBreakpoint::~CommandObjectMultiwordBreakpoint() = default;

void CommandObjectMultiwordBreakpoint::VerifyIDs(
    Args &args, Target *target, bool allow_locations,
    CommandReturnObject &result, BreakpointIDList *valid_ids,
    BreakpointName::Permissions ::PermissionKinds purpose) {
  // args can be strings representing 1). integers (for breakpoint ids)
  //                                  2). the full breakpoint & location
  //                                  canonical representation
  //                                  3). the word "to" or a hyphen,
  //                                  representing a range (in which case there
  //                                      had *better* be an entry both before &
  //                                      after of one of the first two types.
  //                                  4). A breakpoint name
  // If args is empty, we will use the last created breakpoint (if there is
  // one.)

  Args temp_args;

  if (args.empty()) {
    if (target->GetLastCreatedBreakpoint()) {
      valid_ids->AddBreakpointID(BreakpointID(
          target->GetLastCreatedBreakpoint()->GetID(), LLDB_INVALID_BREAK_ID));
      result.SetStatus(eReturnStatusSuccessFinishNoResult);
    } else {
      result.AppendError(
          "No breakpoint specified and no last created breakpoint.");
      result.SetStatus(eReturnStatusFailed);
    }
    return;
  }

  // Create a new Args variable to use; copy any non-breakpoint-id-ranges stuff
  // directly from the old ARGS to the new TEMP_ARGS.  Do not copy breakpoint
  // id range strings over; instead generate a list of strings for all the
  // breakpoint ids in the range, and shove all of those breakpoint id strings
  // into TEMP_ARGS.

  BreakpointIDList::FindAndReplaceIDRanges(args, target, allow_locations,
                                           purpose, result, temp_args);

  // NOW, convert the list of breakpoint id strings in TEMP_ARGS into an actual
  // BreakpointIDList:

  valid_ids->InsertStringArray(temp_args.GetArgumentArrayRef(), result);

  // At this point,  all of the breakpoint ids that the user passed in have
  // been converted to breakpoint IDs and put into valid_ids.

  if (result.Succeeded()) {
    // Now that we've converted everything from args into a list of breakpoint
    // ids, go through our tentative list of breakpoint id's and verify that
    // they correspond to valid/currently set breakpoints.

    const size_t count = valid_ids->GetSize();
    for (size_t i = 0; i < count; ++i) {
      BreakpointID cur_bp_id = valid_ids->GetBreakpointIDAtIndex(i);
      Breakpoint *breakpoint =
          target->GetBreakpointByID(cur_bp_id.GetBreakpointID()).get();
      if (breakpoint != nullptr) {
        const size_t num_locations = breakpoint->GetNumLocations();
        if (static_cast<size_t>(cur_bp_id.GetLocationID()) > num_locations) {
          StreamString id_str;
          BreakpointID::GetCanonicalReference(
              &id_str, cur_bp_id.GetBreakpointID(), cur_bp_id.GetLocationID());
          i = valid_ids->GetSize() + 1;
          result.AppendErrorWithFormat(
              "'%s' is not a currently valid breakpoint/location id.\n",
              id_str.GetData());
          result.SetStatus(eReturnStatusFailed);
        }
      } else {
        i = valid_ids->GetSize() + 1;
        result.AppendErrorWithFormat(
            "'%d' is not a currently valid breakpoint ID.\n",
            cur_bp_id.GetBreakpointID());
        result.SetStatus(eReturnStatusFailed);
      }
    }
  }
}
