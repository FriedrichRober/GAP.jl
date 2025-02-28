/*
 * JuliaInterface: Interface to julia
 */

#include "JuliaInterface.h"

#include "calls.h"
#include "convert.h"
#include "sync.h"

#include <src/julia_gc.h>

#include <julia_gcext.h>

jl_module_t * gap_module;

static jl_value_t *    JULIA_ERROR_IOBuffer;
static jl_function_t * JULIA_FUNC_take_inplace;
static jl_function_t * JULIA_FUNC_showerror;
static jl_datatype_t * JULIA_GAPFFE_type;

static jl_datatype_t * gap_datatype_mptr;

static Obj  TheTypeJuliaObject;
static UInt T_JULIA_OBJ;

Obj JULIAINTERFACE_IsJuliaWrapper;
Obj JULIAINTERFACE_JuliaPointer;

void handle_jl_exception(void)
{
    jl_call2(JULIA_FUNC_showerror, JULIA_ERROR_IOBuffer,
             jl_exception_occurred());
    jl_value_t * string_object =
        jl_call1(JULIA_FUNC_take_inplace, JULIA_ERROR_IOBuffer);
    string_object = jl_array_to_string((jl_array_t *)string_object);
    BEGIN_GAP_SYNC();
    ErrorMayQuit("%s", (Int)jl_string_data(string_object), 0);
    END_GAP_SYNC();
}

// note: the following helper expects to be called from inside a
// BEGIN_GAP_SYNC / END_GAP_SYNC section
static jl_module_t * get_module(const char * name)
{
    jl_value_t * module_value =
        jl_get_global(jl_main_module, jl_symbol(name));
    if (!module_value) {
        ErrorQuit("%s not defined", (Int)name, 0);
    }
    if (!jl_is_module(module_value)) {
        ErrorQuit("%s is not a module", (Int)name, 0);
    }
    return (jl_module_t *)module_value;
}

jl_value_t * gap_box_gapffe(Obj value)
{
#if (JULIA_VERSION_MAJOR * 100 + JULIA_VERSION_MINOR) <= 106
    jl_ptls_t ptls = jl_get_ptls_states();
#else
    jl_ptls_t ptls = jl_get_current_task()->ptls;
#endif
    jl_value_t * v = jl_gc_alloc_typed(ptls, sizeof(Obj), JULIA_GAPFFE_type);
    *(Obj *)jl_data_ptr(v) = value;
    return v;
}

Obj gap_unbox_gapffe(jl_value_t * gapffe)
{
    return *(Obj *)jl_data_ptr(gapffe);
}

//
int is_gapffe(jl_value_t * v)
{
    return jl_typeis(v, JULIA_GAPFFE_type);
}

//
int is_gapobj(jl_value_t * v)
{
    return jl_typeis(v, gap_datatype_mptr);
}

/*
 * utilities for wrapped Julia objects and functions
 */
static Obj JuliaObjCopyFunc(Obj obj, Int mut)
{
    /* always immutable in GAP, so nothing to do */
    return obj;
}

static void JuliaObjCleanFunc(Obj obj)
{
}

static Int JuliaObjIsMutableFunc(Obj obj)
{
    /* always immutable as GAP object */
    return 0L;
}

inline int IS_JULIA_OBJ(Obj o)
{
    return TNUM_OBJ(o) == T_JULIA_OBJ;
}

jl_value_t * GET_JULIA_OBJ(Obj o)
{
    return (jl_value_t *)(CONST_ADDR_OBJ(o)[0]);
}

static Obj JuliaObjectTypeFunc(Obj o)
{
    return TheTypeJuliaObject;
}

Obj NewJuliaObj(jl_value_t * v)
{
    if (is_gapobj(v))
        return (Obj)v;
    JL_GC_PUSH1(&v);
    Obj o = NewBag(T_JULIA_OBJ, 1 * sizeof(Obj));
    ADDR_OBJ(o)[0] = (Obj)v;
    JL_GC_POP();
    return o;
}


jl_function_t * get_function_from_obj_or_string(Obj func)
{
    jl_function_t * f = NULL;
    BEGIN_GAP_SYNC();
    if (IS_JULIA_OBJ(func)) {
        f = (jl_function_t *)GET_JULIA_OBJ(func);
    }
    else if (IsStringConv(func)) {
        // jl_get_function is a thin wrapper for jl_get_global and never
        // throws an exception
        f = jl_get_function(jl_main_module, CONST_CSTR_STRING(func));
        if (f == 0) {
            ErrorMayQuit("Function is not defined in julia", 0, 0);
        }
    }
    else
        ErrorMayQuit("argument is not a julia object or string", 0, 0);
    END_GAP_SYNC();
    return f;
}


void ResetUserHasQUIT(void)
{
    STATE(UserHasQUIT) = 0;
}


// HACK: this helper function is called from Julia to override the parent
// module and name of the type used for GAP objects. We used to achieve the
// same via pure Julia like this:
//
//    GapObj.name.module = GAP
//    GapObj.name.name = :GapObj
//
// However in recent Julia 1.8-DEV builds, a feature was introduced to mark
// individual fields of mutable structs const, and this was used to make those
// field of type `TypeName` const, so that code doesn't work anymore. For now,
// it is still safe to override the type on the C kernel level, but we should
// start thinking about long-term alternatives.
void OverrideTypeNameAndModule(jl_value_t * type,
                               jl_value_t * module,
                               jl_value_t * name)
{
    if (!jl_is_datatype(type))
        jl_error("<type> is not a DataType");
    if (!jl_is_module(module))
        jl_error("<module> is not a module");
    if (!jl_is_symbol(name))
        jl_error("<name> is not a symbol");

    jl_datatype_t * t = (jl_datatype_t *)type;
    t->name->name = (jl_sym_t *)name;
    t->name->module = (jl_module_t *)module;
}


/*
 * Returns the function from the Object <func>
 * or the function with name <func> from
 * the Julia main module.
 */
static Obj Func_JuliaFunction(Obj self, Obj func)
{
    jl_function_t * f = get_function_from_obj_or_string(func);
    return WrapJuliaFunc(f);
}

/*
 * Returns the function with name <funcName> from the Julia module with
 * name <moduleName>.
 */
static Obj Func_JuliaFunctionByModule(Obj self, Obj funcName, Obj moduleName)
{
    BEGIN_GAP_SYNC();
    RequireStringRep("_JuliaFunctionByModule", funcName);
    RequireStringRep("_JuliaFunctionByModule", moduleName);

    jl_module_t * m = get_module(CONST_CSTR_STRING(moduleName));
    // jl_get_function is a thin wrapper for jl_get_global and never throws
    // an exception
    jl_function_t * f = jl_get_function(m, CONST_CSTR_STRING(funcName));
    if (f == 0)
        ErrorMayQuit("Function %g.%g is not defined in julia",
                     (Int)moduleName, (Int)funcName);
    END_GAP_SYNC();
    return WrapJuliaFunc(f);
}

// Export 'IS_JULIA_FUNC' to the GAP level.
static Obj FuncIS_JULIA_FUNC(Obj self, Obj obj)
{
    return IS_JULIA_FUNC(obj) ? True : False;
}

// Executes the string <string> in the current julia session.
static Obj FuncJuliaEvalString(Obj self, Obj string)
{
    BEGIN_GAP_SYNC();
    RequireStringRep("JuliaEvalString", string);

    jl_value_t * result = jl_eval_string(CONST_CSTR_STRING(string));
    END_GAP_SYNC();
    if (jl_exception_occurred()) {
        handle_jl_exception();
    }
    return gap_julia(result);
}

// Returns a julia object GAP object that holds the pointer to a julia symbol
// :<name>.
static Obj FuncJuliaSymbol(Obj self, Obj name)
{
    BEGIN_GAP_SYNC();
    RequireStringRep("JuliaSymbol", name);

    // jl_symbol never throws an exception and always returns a valid
    // result, so no need for extra checks.
    jl_sym_t * julia_symbol = jl_symbol(CONST_CSTR_STRING(name));
    END_GAP_SYNC();
    return NewJuliaObj((jl_value_t *)julia_symbol);
}

// Sets the value of the julia identifier <name> to the <val>.
// This function is for debugging purposes.
static Obj FuncJuliaSetVal(Obj self, Obj name, Obj val)
{
    BEGIN_GAP_SYNC();
    RequireStringRep("JuliaSetVal", name);

    jl_value_t * julia_obj = julia_gap(val);
    jl_sym_t *   julia_symbol = jl_symbol(CONST_CSTR_STRING(name));
    END_GAP_SYNC();
    jl_set_global(jl_main_module, julia_symbol, julia_obj);
    return 0;
}

// Returns the julia object GAP object that holds a pointer to the value
// currently bound to the julia identifier <name>.
static Obj Func_JuliaGetGlobalVariable(Obj self, Obj name)
{
    BEGIN_GAP_SYNC();
    RequireStringRep("_JuliaGetGlobalVariable", name);

    jl_sym_t * symbol = jl_symbol(CONST_CSTR_STRING(name));
    END_GAP_SYNC();
    if (!jl_boundp(jl_main_module, symbol)) {
        return Fail;
    }
    jl_value_t * value = jl_get_global(jl_main_module, symbol);
    return gap_julia(value);
}

// Returns the julia object GAP object that holds a pointer to the value
// currently bound to the julia identifier <moduleName>.<name>.
static Obj Func_JuliaGetGlobalVariableByModule(Obj self, Obj name, Obj module)
{
    BEGIN_GAP_SYNC();
    RequireStringRep("_JuliaGetGlobalVariableByModule", name);

    jl_module_t * m = 0;
    if (IS_JULIA_OBJ(module)) {
        m = (jl_module_t *)GET_JULIA_OBJ(module);
        if (!jl_is_module(m))
            m = 0;
    }
    else if (IsStringConv(module)) {
        m = get_module(CONST_CSTR_STRING(module));
    }
    if (!m) {
        ErrorMayQuit("_JuliaGetGlobalVariableByModule: <module> must be a "
                     "string or a Julia module",
                     0, 0);
    }
    jl_sym_t * symbol = jl_symbol(CONST_CSTR_STRING(name));
    END_GAP_SYNC();
    if (!jl_boundp(m, symbol)) {
        return Fail;
    }
    jl_value_t * value = jl_get_global(m, symbol);
    return gap_julia(value);
}

static Obj Func_JuliaGetGapModule(Obj self)
{
    return NewJuliaObj((jl_value_t *)gap_module);
}

// Mark the Julia pointer inside the GAP JuliaObj
static void MarkJuliaObject(Bag bag)
{
#ifdef DEBUG_MASTERPOINTERS
    MarkJuliaObjSafe((void *)GET_JULIA_OBJ(bag));
#else
    MarkJuliaObj((void *)GET_JULIA_OBJ(bag));
#endif
}

// Table of functions to export
static StructGVarFunc GVarFuncs[] = {
    GVAR_FUNC(_JuliaFunction, 1, "string"),
    GVAR_FUNC(_JuliaFunctionByModule, 2, "funcName, moduleName"),
    GVAR_FUNC(IS_JULIA_FUNC, 1, "obj"),
    GVAR_FUNC(JuliaEvalString, 1, "string"),
    GVAR_FUNC(JuliaSetVal, 2, "name,val"),
    GVAR_FUNC(_JuliaGetGlobalVariable, 1, "name"),
    GVAR_FUNC(_JuliaGetGlobalVariableByModule, 2, "name, module"),
    GVAR_FUNC(JuliaSymbol, 1, "name"),
    GVAR_FUNC(_JuliaGetGapModule, 0, ""),
    { 0 } /* Finish with an empty entry */

};

/****************************************************************************
**
*F  InitKernel( <module> )  . . . . . . . . initialise kernel data structures
*/
static Int InitKernel(StructInitInfo * module)
{
    if (!gap_module) {
        ErrorMayQuit("gap_module was not set", 0, 0);
    }

    JULIA_GAPFFE_type =
        (jl_datatype_t *)jl_get_global(gap_module, jl_symbol("FFE"));
    if (!JULIA_GAPFFE_type) {
        ErrorMayQuit("Could not locate the GAP.FFE datatype", 0, 0);
    }

    InitGapSync();

    // init filters and functions
    InitHdlrFuncsFromTable(GVarFuncs);

    InitCopyGVar("TheTypeJuliaObject", &TheTypeJuliaObject);

    T_JULIA_OBJ = RegisterPackageTNUM("JuliaObject", JuliaObjectTypeFunc);

    InitMarkFuncBags(T_JULIA_OBJ, &MarkJuliaObject);

    CopyObjFuncs[T_JULIA_OBJ] = &JuliaObjCopyFunc;
    CleanObjFuncs[T_JULIA_OBJ] = &JuliaObjCleanFunc;
    IsMutableObjFuncs[T_JULIA_OBJ] = &JuliaObjIsMutableFunc;

    // Initialize necessary variables for error handling
    JULIA_ERROR_IOBuffer =
        jl_call0(jl_get_function(jl_base_module, "IOBuffer"));
    GAP_ASSERT(JULIA_ERROR_IOBuffer);
    // store the IO buffer object to protect it from being garbage collected
    jl_set_const(gap_module, jl_symbol("error_buffer"), JULIA_ERROR_IOBuffer);

    JULIA_FUNC_take_inplace = jl_get_function(jl_base_module, "take!");
    GAP_ASSERT(JULIA_FUNC_take_inplace);

    JULIA_FUNC_showerror = jl_get_function(jl_base_module, "showerror");
    GAP_ASSERT(JULIA_FUNC_showerror);

    // paranoia: verify that Julia's GMP has the BITS_PER_LIMB we expect
    jl_module_t * gmp_module =
        (jl_module_t *)jl_get_global(jl_base_module, jl_symbol("GMP"));
    GAP_ASSERT(gmp_module);
    int bits_per_limb =
        jl_unbox_long(jl_get_global(gmp_module, jl_symbol("BITS_PER_LIMB")));
    if (sizeof(UInt) * 8 != bits_per_limb) {
        Panic("GMP limb size is %d in GAP and %d in Julia",
              (int)sizeof(UInt) * 8, bits_per_limb);
    }

    // import mptr type from GAP, by getting the Julia type of any GAP object
    gap_datatype_mptr = (jl_datatype_t *)jl_typeof(True);
    GAP_ASSERT(gap_datatype_mptr);

    ImportFuncFromLibrary("IsJuliaWrapper", &JULIAINTERFACE_IsJuliaWrapper);
    ImportFuncFromLibrary("JuliaPointer", &JULIAINTERFACE_JuliaPointer);

    // return success
    return 0;
}

/****************************************************************************
**
*F  InitLibrary( <module> ) . . . . . . .  initialise library data structures
*/
static Int InitLibrary(StructInitInfo * module)
{
    // init filters and functions
    InitGVarFuncsFromTable(GVarFuncs);

    // return success
    return 0;
}

/****************************************************************************
**
*F  Init__Dynamic() . . . . . . . . . . . . . . . . . table of init functions
*/
StructInitInfo * Init__Dynamic(void)
{
    static StructInitInfo module = {
        .type = MODULE_DYNAMIC,
        .name = "JuliaInterface",
        .initKernel = InitKernel,
        .initLibrary = InitLibrary,
    };
    return &module;
}
