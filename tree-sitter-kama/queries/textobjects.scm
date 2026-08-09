; Helix textobjects — `mif` / `maf` (function), `mic` / `mac` (class), and so on.
(function_declaration body: (block) @function.inside) @function.around
(method_declaration body: (block) @function.inside) @function.around
(constructor_declaration body: (block) @function.inside) @function.around
(destructor_declaration body: (block) @function.inside) @function.around

(type_declaration body: (class_body) @class.inside) @class.around
(enum_declaration body: (enum_body) @class.inside) @class.around

(parameter_list ((parameter) @parameter.inside . ","? @parameter.around)) @parameter.around
(argument_list ((argument) @parameter.inside . ","? @parameter.around)) @parameter.around

[(line_comment) (block_comment)] @comment.inside
