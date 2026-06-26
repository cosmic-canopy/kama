# Legacy LLVM backend (reference only)

These files are the original LLVM IR backend, kept for reference after the
pivot to a C transpiler. They are NOT part of the build. Mine them for the
old type mapping (`getTypeFromIdentifier`), default-value logic
(`createDefault`), and the scope/symbol-table design when implementing the
semantic pass and later milestones.
