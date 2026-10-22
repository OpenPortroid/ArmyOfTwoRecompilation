// hlsl_translator.h - Xenos microcode -> HLSL (Shader Model 5.1) for the
// native renderer. Built on the SDK's ucode parser (src/gpu/xenos).
//
// Binding model (must match the root signature in renderer.cpp):
//   b0          float constants of the stage (256 x float4, SQ_*_CONST base)
//   b1          XeSystemConstants (hlsl_shared.h)
//   t0, space1  guest physical memory as ByteAddressBuffer (vertex fetch)
//   t# / s#     texture/sampler of texture binding # (Shader::texture_bindings)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/translator.h>

namespace ao2::gpu
{
  // Per-draw variant of a shader translation.
  union HlslModification
  {
    uint64_t value = 0;
    struct
    {
      // Temporary registers allocated by SQ_PROGRAM_CNTL (num_reg + 1).
      uint64_t register_count : 7;
      // PS: register that receives ps_param_gen, 31 = none.
      uint64_t param_gen_register : 5;
      // VS: 0 = regular, 1 = rectangle list via GS, 2 = point sprite via GS.
      uint64_t host_vertex_type : 2;
      // PS: 1 + the texture binding whose 2D fetch coordinates (0 to 1 over
      // the guest texture) are written to color 0 instead of the colors, 0 =
      // none (the menu probe, renderer.cpp).
      uint64_t probe_binding : 5;
      // PS: the alpha test is on (RB_COLORCONTROL, not "always"): only then
      // has the shader a discard for it. A discard anywhere in a shader keeps
      // the GPU from writing depth before the shader runs, so the opaque
      // passes lost their early depth rejection to an alpha test they never
      // use.
      uint64_t alpha_test : 1;
    };
  };

  class HlslShaderTranslator : public rex::graphics::ShaderTranslator
  {
  public:
    HlslShaderTranslator() = default;

    // Translates an analyzed shader; the HLSL text is stored as the
    // translation's binary.
    bool Translate(rex::graphics::Shader::Translation &translation)
    {
      return TranslateAnalyzedShader(translation);
    }

    // Vertex fetch whose x/y the last translated vertex shader exports
    // unchanged as the position (D3D's clear helper) - only then the vertex
    // data tells which pixels a rectangle list covers.
    struct PositionFetch
    {
      bool valid = false;
      uint32_t fetch_constant = 0;
      int32_t offset = 0;        // dwords
      uint32_t stride = 0;       // dwords
      rex::graphics::xenos::VertexFormat format = rex::graphics::xenos::VertexFormat::kUndefined;
    };
    PositionFetch position_passthrough() const
    {
      return position_state_ == 1 ? position_fetch_ : PositionFetch{};
    }

  protected:
    uint32_t GetModificationRegisterCount() const override;
    void Reset() override;
    void StartTranslation() override;
    std::vector<uint8_t> CompleteTranslation() override;

    void PreProcessControlFlowInstructions(
        std::vector<rex::graphics::ucode::ControlFlowInstruction> instrs) override;
    void ProcessLabel(uint32_t cf_index) override;
    void ProcessExecInstructionBegin(const rex::graphics::ParsedExecInstruction &instr) override;
    void ProcessExecInstructionEnd(const rex::graphics::ParsedExecInstruction &instr) override;
    void ProcessLoopStartInstruction(const rex::graphics::ParsedLoopStartInstruction &instr) override;
    void ProcessLoopEndInstruction(const rex::graphics::ParsedLoopEndInstruction &instr) override;
    void ProcessCallInstruction(const rex::graphics::ParsedCallInstruction &instr) override;
    void ProcessReturnInstruction(const rex::graphics::ParsedReturnInstruction &instr) override;
    void ProcessJumpInstruction(const rex::graphics::ParsedJumpInstruction &instr) override;
    void ProcessVertexFetchInstruction(const rex::graphics::ParsedVertexFetchInstruction &instr) override;
    void ProcessTextureFetchInstruction(const rex::graphics::ParsedTextureFetchInstruction &instr) override;
    void ProcessAluInstruction(const rex::graphics::ParsedAluInstruction &instr,
                               uint8_t memexport_eM_potentially_written_before) override;

  private:
    HlslModification modification() const;

    void Line(const std::string &s);
    void Open(const std::string &s);
    void Close();

    std::string RegisterName(uint32_t index,
                             rex::graphics::InstructionStorageAddressingMode mode) const;
    std::string Operand(const rex::graphics::InstructionOperand &op) const;
    std::string OperandComponent(const rex::graphics::InstructionOperand &op, uint32_t component) const;
    // `nonfinite`: components of `value` that may be infinity or NaN (see
    // nonfinite_).
    void StoreResult(const rex::graphics::InstructionResult &result, const std::string &value,
                     uint32_t nonfinite = 0xF);
    uint32_t OperandNonfinite(const rex::graphics::InstructionOperand &op) const;

    void OpenExecConditional(rex::graphics::ParsedExecInstruction::Type type, uint32_t bool_index,
                             bool condition);
    void CloseExecConditional();
    void SetInstructionPredication(bool predicated, bool condition);
    void ClosePredication();
    std::string BoolConstantTest(uint32_t index) const;
    std::string LoopConstant(uint32_t index) const;

    void EmitVectorOp(const rex::graphics::ParsedAluInstruction &instr, bool &predicate_written,
                      uint32_t &nonfinite);
    void EmitScalarOp(const rex::graphics::ParsedAluInstruction &instr, bool &predicate_written);

    std::string body_;
    int indent_ = 0;
    bool exec_open_ = false;
    bool predicate_open_ = false;
    bool predicate_condition_ = false;
    uint32_t temp_counter_ = 0;
    // A `while (true)` with `continue` is only emitted for loops and backward
    // jumps - FXC fails on gradients in such loops, so forward-only control
    // flow is a flat sequence of `if (pc <= label)` blocks.
    bool use_loop_ = false;
    uint32_t current_label_ = 0;

    void ReopenLabelGuard();

    // Position pass-through analysis (see position_passthrough).
    void TrackPositionWrite(const rex::graphics::ParsedAluInstruction &instr);
    void TrackRegisterWrite(const rex::graphics::InstructionResult &result);
    PositionFetch position_fetch_;
    int32_t position_register_ = -1; // register with the fetched x/y; -1 none yet, -2 lost
    int position_state_ = 0;         // 0 not exported yet, 1 pass-through, -1 computed
    bool conditional_exec_ = false;

    // Which register components may hold infinity or NaN, in program order
    // (only ever set: a later finite write may be predicated). Only there is
    // Shader Model 3's 0 * x = 0 different from IEEE multiplication, so only
    // there multiplications need xe_mul - its select also keeps FXC from
    // fusing mul + add. Sources: rcp, rsq, log, exp, sqrt, vertex fetch,
    // relative addressing; constants, interpolators and texture fetches count
    // as finite. Off (always xe_mul) with loops or backward jumps.
    uint8_t nonfinite_[256] = {};
    bool ps_nonfinite_ = false;
    bool track_finite_ = false;
    // The instruction being translated always runs (no predicate, no
    // conditional exec, no label a jump may skip to yet): its writes replace
    // what nonfinite_ / ps_nonfinite_ said instead of adding to it.
    bool unconditional_ = false;
  };
} // namespace ao2::gpu
