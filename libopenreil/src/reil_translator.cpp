#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <iostream>
#include <string>
#include <algorithm>

extern "C" 
{ 
#include "libvex.h" 
}

// libasmir includes
#include "irtoir.h"
#include "irtoir-internal.h"

// libasmir architecture specific
#include "irtoir-i386.h"

// defined in irtoir.cpp
extern string uTag;
extern string sTag;

// OpenREIL includes
#include "libopenreil.h"
#include "reil_translator.h"

using namespace std;

#include "disasm.h"

const char *reil_inst_name[] = 
{
    "NONE", "UNK", "JCC", 
    "STR", "STM", "LDM", 
    "ADD", "SUB", "NEG", "MUL", "DIV", "MOD", "SMUL", "SDIV", "SMOD", 
    "SHL", "SHR", "AND", "OR", "XOR", "NOT",
    "EQ", "LT"
};

reil_op_t reil_inst_map_binop[] = 
{
    /* PLUS     */ I_ADD, 
    /* MINUS    */ I_SUB,   
    /* TIMES    */ I_MUL,  
    /* DIVIDE   */ I_DIV,
    /* MOD      */ I_MOD,      
    /* LSHIFT   */ I_SHL,   
    /* RSHIFT   */ I_SHR,  
    /* ARSHIFT  */ I_NONE,
    /* LROTATE  */ I_NONE,  
    /* RROTATE  */ I_NONE,  
    /* LOGICAND */ I_AND, 
    /* LOGICOR  */ I_OR,
    /* BITAND   */ I_AND,  
    /* BITOR    */ I_OR,       
    /* XOR      */ I_XOR,      
    /* EQ       */ I_EQ,
    /* NEQ      */ I_NONE,  
    /* GT       */ I_NONE,       
    /* LT       */ I_LT,       
    /* GE       */ I_NONE,
    /* LE       */ I_NONE, 
    /* SDIVIDE  */ I_SDIV, 
    /* SMOD     */ I_SMOD   
};

reil_op_t reil_inst_map_unop[] = 
{
    /* NEG      */ I_NEG,
    /* NOT      */ I_NOT 
};

template<class T>
string _to_string(T i)
{
    stringstream s;
    s << i;
    return s.str();
}

void reil_assert(bool condition, string reason)
{
    if (!condition) throw CReilTranslatorException(reason);
}

#define RELATIVE ((exp_type_t)((uint32_t)EXTENSION + 1))

class Relative : public Exp 
{
public:
    Relative(reg_t t, const_val_t val); 
    Relative(const Relative& other);
    virtual Relative *clone() const;
    virtual ~Relative() {}
    static void destroy(Constant *expr);

    virtual string tostring() const;
    virtual void accept(IRVisitor *v) { }
    reg_t typ;
    const_val_t val;  
};

Relative::Relative(reg_t t, const_val_t v)
  : Exp(RELATIVE), typ(t), val(v) { }

Relative::Relative(const Relative& other)
  : Exp(RELATIVE), typ(other.typ), val(other.val) { }

Relative *Relative::clone() const
{
    return new Relative(*this);
}

string Relative::tostring() const
{
    return string("$+") + _to_string(val);
}

void Relative::destroy(Constant *expr)
{
    assert(expr);
    delete expr;
}

CReilFromBilTranslator::CReilFromBilTranslator(VexArch arch, reil_inst_handler_t handler, void *context)
{
    guest = arch;
    inst_handler = handler;
    inst_handler_context = context;
    reset_state(NULL);
}

CReilFromBilTranslator::~CReilFromBilTranslator()
{
    
}

void CReilFromBilTranslator::reset_state(bap_block_t *block)
{
    tempreg_bap.clear();
    
    current_block = block;
    current_stmt = -1;

    tempreg_count = inst_count = 0;
    skip_eflags = false;    
}

int32_t CReilFromBilTranslator::tempreg_find(string name)
{
    vector<TEMPREG_BAP>::iterator it;

    // find temporary registry number by BAP temporary registry name
    for (it = tempreg_bap.begin(); it != tempreg_bap.end(); ++it)
    {
        if (it->second == name)
        {
            return it->first;
        }
    }

    return -1;
}

int32_t CReilFromBilTranslator::tempreg_alloc(void)
{
    while (true)
    {
        vector<TEMPREG_BAP>::iterator it;
        bool found = false;
        int32_t ret = tempreg_count;

        // check if temporary registry number was reserved for BAP registers
        for (it = tempreg_bap.begin(); it != tempreg_bap.end(); ++it)
        {
            if (it->first == tempreg_count)
            {
                found = true;
                break;
            }
        }   

        tempreg_count += 1;     
        if (!found) return ret;
    }

    reil_assert(0, "error while allocating temp registry");
    return -1;
}

string CReilFromBilTranslator::tempreg_get_name(int32_t tempreg_num)
{
    char number[15];
    sprintf(number, "%.2d", tempreg_num);

    string tempreg_name = string("V_");
    tempreg_name += number;

    return tempreg_name;
}

string CReilFromBilTranslator::tempreg_get(string name)
{
    // lookup for BAP temporary registry alias
    int32_t tempreg_num = tempreg_find(name);
    if (tempreg_num == -1)
    {
        // there is no alias for this registry, create it
        tempreg_num = tempreg_alloc();
        tempreg_bap.push_back(make_pair(tempreg_num, name));

#ifdef DBG_TEMPREG

        printf("Temp reg %d reserved for %s\n", tempreg_num, name.c_str());
#endif

    }
    else
    {

#ifdef DBG_TEMPREG

        printf("Temp reg %d found for %s\n", tempreg_num, name.c_str());   
#endif

    }

    return tempreg_get_name(tempreg_num);
}

uint64_t CReilFromBilTranslator::convert_special(Special *special)
{
    if (special->special == "call")
    {
        return IOPT_CALL;
    }
    else if (special->special == "ret")
    {
        return IOPT_RET;
    }

    return 0;
}

reil_size_t CReilFromBilTranslator::convert_operand_size(reg_t typ)
{
    switch (typ)
    {
    case REG_1: return U1;
    case REG_8: return U8;
    case REG_16: return U16;
    case REG_32: return U32;
    case REG_64: return U64;
    default: reil_assert(0, "invalid operand size");
    }    
}

reg_t CReilFromBilTranslator::convert_operand_size(reil_size_t size)
{
    switch (size)
    {
    case U1: return REG_1;
    case U8: return REG_8;
    case U16: return REG_16;
    case U32: return REG_32;
    case U64: return REG_64;
    default: reil_assert(0, "invalid operand size");
    }    
}

void CReilFromBilTranslator::convert_operand(Exp *exp, reil_arg_t *reil_arg)
{
    if (exp == NULL)
    {
        memset(reil_arg, 0, sizeof(reil_arg_t));
        reil_arg->type = A_NONE;
        return;
    }

    reil_assert(
        exp->exp_type == TEMP || exp->exp_type == CONSTANT || exp->exp_type == RELATIVE,
        "invalid expression type");

    if (exp->exp_type == CONSTANT)
    {
        // special handling for canstants
        Constant *constant = (Constant *)exp;
        reil_arg->type = A_CONST;
        reil_arg->size = convert_operand_size(constant->typ);
        reil_arg->val = constant->val;
        return;
    }

    Temp *temp = (Temp *)exp;    
    string ret = temp->name;
    const char *c_name = ret.c_str();

    if (strncmp(c_name, "R_", 2) && strncmp(c_name, "V_", 2))
    {
        // this is a BAP temporary registry
        ret = tempreg_get(ret);
    }

    if (!strncmp(c_name, "R_", 2))
    {
        // architecture register
        reil_arg->type = A_REG;
        reil_arg->size = convert_operand_size(temp->typ);
        strncpy(reil_arg->name, ret.c_str(), REIL_MAX_NAME_LEN - 1);
    }
    else
    {
        // temporary register
        reil_arg->type = A_TEMP;
        reil_arg->size = convert_operand_size(temp->typ);
        strncpy(reil_arg->name, ret.c_str(), REIL_MAX_NAME_LEN - 1);
    }

    if (!strcmp(reil_arg->name, "R_EFLAGS") && !skip_eflags)
    {        
        vector<Stmt *> set_eflags_stmt;
        vector<Stmt *>::iterator it;

        set_eflags_bits(
            &set_eflags_stmt, 
            mk_reg("CF", REG_1), 
            mk_reg("PF", REG_1), 
            mk_reg("AF", REG_1), 
            mk_reg("ZF", REG_1), 
            mk_reg("SF", REG_1), 
            mk_reg("OF", REG_1)
        );

        skip_eflags = true;

        // enumerate expressions that sets EFLAGS value        
        for (it = set_eflags_stmt.begin(); it != set_eflags_stmt.end(); ++it)
        {
            process_bil_stmt(*it, 0);
        }

        skip_eflags = false;
    }
}

Exp *CReilFromBilTranslator::temp_operand(reg_t typ, reil_inum_t inum)
{
    char buff[MAX_REG_NAME_LEN];
    sprintf(buff, "V_REIL_TMP_%d", inum);

    return new Temp(typ, tempreg_get(buff));
}

void CReilFromBilTranslator::process_reil_inst(reil_inst_t *reil_inst)
{
    if (inst_handler)
    {
        if (reil_inst->inum == 0 && current_raw_info)
        {
            // first IR instruction must contain extended information about machine code
            reil_inst->raw_info.data = current_raw_info->data;
            reil_inst->raw_info.str_mnem = current_raw_info->str_mnem;
            reil_inst->raw_info.str_op = current_raw_info->str_op;
        }

        // call user-specified REIL instruction handler
        inst_handler(reil_inst, inst_handler_context);
    }
}

void CReilFromBilTranslator::free_bil_exp(Exp *exp)
{
    if (exp) 
    {
        // free temp expression that was returned by process_bil_exp()
        Temp::destroy(reinterpret_cast<Temp *>(exp));
    }
}

Exp *CReilFromBilTranslator::process_bil_exp(Exp *exp)
{
    Exp *ret = exp;

    if (exp->exp_type != TEMP && exp->exp_type != CONSTANT)
    {
        reil_assert(
            exp->exp_type == BINOP || exp->exp_type == UNOP || exp->exp_type == CAST,
            "invaid expression type");

        // expand complex expression and store result to the new temporary value
        return process_bil_inst(I_STR, 0, NULL, exp);
    }    

    return NULL;
}

reil_const_t reil_cast_bits(reil_size_t size)
{
    switch (size)
    {
    case U1: return 1;
    case U8: return 8;
    case U16: return 16;
    case U32: return 32;
    case U64: return 64;
    default: reil_assert(0, "invalid size");
    }
}

reil_const_t reil_cast_mask(reil_size_t size)
{
    switch (size)
    {
    case U1: return 0x1;
    case U8: return 0xff;
    case U16: return 0xffff;
    case U32: return 0xffffffff;
    case U64: return 0xffffffffffffffff;
    default: reil_assert(0, "invalid size");
    }
}

reil_const_t reil_cast_mask_sign(reil_size_t size)
{
    switch (size)
    {
    case U1: return 0x1;
    case U8: return 0x80;
    case U16: return 0x8000;
    case U32: return 0x80000000;
    case U64: return 0x8000000000000000;
    default: reil_assert(0, "invalid size");
    }
}

reil_const_t reil_cast_high(reil_size_t size)
{
    switch (size)
    {
    case U16: return 8;
    case U32: return 16;
    case U64: return 32;
    default: reil_assert(0, "invalid size");
    }
}

#define COPY_ARG(_dst_, _src_) memcpy((_dst_), (_src_), sizeof(reil_arg_t))

#define NEW_INST(_op_, _inum_)                          \
                                                        \
    memset(&new_inst, 0, sizeof(new_inst));             \
    new_inst.op = (_op_);                               \
    new_inst.raw_info.addr = current_raw_info->addr;    \
    new_inst.raw_info.size = current_raw_info->size;    \
                                                        \
    new_inst.inum = (_inum_);                           \
    inst_count += 1;    

void CReilFromBilTranslator::process_bil_arshift(reil_inst_t *reil_inst)
{
    reil_inst_t new_inst;
    reil_size_t size_dst = reil_inst->c.size;

    Exp *tmp_0 = temp_operand(convert_operand_size(reil_inst->a.size), reil_inst->inum);            

    // get sign bit of the source value
    // AND src, mask, tmp_0
    NEW_INST(I_AND, reil_inst->inum);
    COPY_ARG(&new_inst.a, &reil_inst->a);
    new_inst.b.type = A_CONST;
    new_inst.b.size = new_inst.a.size;
    new_inst.b.val = reil_cast_mask_sign(new_inst.b.size);
    convert_operand(tmp_0, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    Exp *tmp_1 = temp_operand(REG_1, reil_inst->inum);

    // check if sign bit is zero
    // EQ tmp_0, 0, tmp_1
    NEW_INST(I_EQ, reil_inst->inum);
    convert_operand(tmp_0, &new_inst.a);
    new_inst.b.type = A_CONST;
    new_inst.b.size = new_inst.a.size;
    new_inst.b.val = 0;
    convert_operand(tmp_1, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    Exp *tmp_2 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);

    // extend value size
    // OR tmp_1, 0, tmp_2
    NEW_INST(I_OR, reil_inst->inum);
    convert_operand(tmp_1, &new_inst.a);
    new_inst.b.type = A_CONST;
    new_inst.b.size = size_dst;
    new_inst.b.val = 0;
    convert_operand(tmp_2, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    Exp *tmp_3 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);

    // set all bits if sign bit of source value was set
    // SUB tmp_2, 1, tmp_3
    NEW_INST(I_SUB, reil_inst->inum);
    convert_operand(tmp_2, &new_inst.a);
    new_inst.b.type = A_CONST;
    new_inst.b.size = size_dst;
    new_inst.b.val = 1;
    convert_operand(tmp_3, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    Exp *tmp_4 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);

    // calculate left shift size for mask
    // SUB digits, shift, tmp_4
    NEW_INST(I_SUB, reil_inst->inum);
    new_inst.a.type = A_CONST;
    new_inst.a.size = size_dst;
    new_inst.a.val = reil_cast_bits(size_dst);
    COPY_ARG(&new_inst.b, &reil_inst->b);
    convert_operand(tmp_4, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    Exp *tmp_5 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);

    // make higher bits mask
    // SHL tmp_3, tmp_4, tmp_5
    NEW_INST(I_SHL, reil_inst->inum);
    convert_operand(tmp_3, &new_inst.a);
    convert_operand(tmp_4, &new_inst.b);
    convert_operand(tmp_5, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    Exp *tmp_6 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);

    // calculate lower bits of destination value
    // SHR src, shift, tmp_6
    NEW_INST(I_SHR, reil_inst->inum);
    COPY_ARG(&new_inst.a, &reil_inst->a);
    COPY_ARG(&new_inst.b, &reil_inst->b);
    convert_operand(tmp_6, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    // set higher bits of destination value
    // OR tmp_5, tmp_6, dst
    reil_inst->op = I_OR;            
    convert_operand(tmp_5, &reil_inst->a);
    convert_operand(tmp_6, &reil_inst->b);

    free_bil_exp(tmp_0);
    free_bil_exp(tmp_1);
    free_bil_exp(tmp_2);
    free_bil_exp(tmp_3);
    free_bil_exp(tmp_4);
    free_bil_exp(tmp_5);
    free_bil_exp(tmp_6);
}

void CReilFromBilTranslator::process_bil_neq(reil_inst_t *reil_inst)
{
    reil_inst_t new_inst;
    reil_size_t size_dst = reil_inst->c.size;

    Exp *tmp = temp_operand(convert_operand_size(size_dst), reil_inst->inum);            

    // EQ a, b, tmp
    NEW_INST(I_EQ, reil_inst->inum);
    COPY_ARG(&new_inst.a, &reil_inst->a);
    COPY_ARG(&new_inst.b, &reil_inst->b);
    convert_operand(tmp, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    // NOT tmp, c
    reil_inst->op = I_NOT;   
    convert_operand(tmp, &new_inst.a);
    convert_operand(NULL, &new_inst.b);

    free_bil_exp(tmp);
}

void CReilFromBilTranslator::process_bil_le(reil_inst_t *reil_inst)
{
    reil_inst_t new_inst;
    reil_size_t size_dst = reil_inst->c.size;

    Exp *tmp_0 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);    

    // EQ a, b, tmp_0
    NEW_INST(I_EQ, reil_inst->inum);
    COPY_ARG(&new_inst.a, &reil_inst->a);
    COPY_ARG(&new_inst.b, &reil_inst->b);
    convert_operand(tmp_0, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    Exp *tmp_1 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);

    // LT a, b, tmp_1
    NEW_INST(I_LT, reil_inst->inum);
    COPY_ARG(&new_inst.a, &reil_inst->a);
    COPY_ARG(&new_inst.b, &reil_inst->b);
    convert_operand(tmp_1, &new_inst.c);

    process_reil_inst(&new_inst);
    reil_inst->inum += 1;

    // OR tmp_0, tmp_1, c
    reil_inst->op = I_OR;   
    convert_operand(tmp_0, &new_inst.a);
    convert_operand(tmp_1, &new_inst.b);

    free_bil_exp(tmp_0);
    free_bil_exp(tmp_1);
}

bool CReilFromBilTranslator::process_bil_cast(Exp *exp, reil_inst_t *reil_inst)
{
    reil_inst_t new_inst;
    Cast *cast = (Cast *)exp;    

    switch (cast->cast_type)
    {
    case CAST_LOW:
        {
            // use low half of the value
            reil_inst->op = I_AND;
            reil_inst->b.type = A_CONST;
            reil_inst->b.size = reil_inst->c.size;
            reil_inst->b.val = reil_cast_mask(reil_inst->c.size);

            return true;
        }

    case CAST_HIGH:
        {
            // use high half of the value
            Exp *tmp = temp_operand(convert_operand_size(reil_inst->a.size), reil_inst->inum);

            NEW_INST(I_SHR, reil_inst->inum);
            COPY_ARG(&new_inst.a, &reil_inst->a);
            new_inst.b.type = A_CONST;
            new_inst.b.size = new_inst.a.size;
            new_inst.b.val = reil_cast_high(new_inst.b.size);
            convert_operand(tmp, &new_inst.c);

            process_reil_inst(&new_inst);
            reil_inst->inum += 1;

            reil_inst->op = I_AND;            
            convert_operand(tmp, &reil_inst->a);            
            reil_inst->b.type = A_CONST;
            reil_inst->b.size = reil_inst->c.size;
            reil_inst->b.val = reil_cast_mask(reil_inst->c.size);

            free_bil_exp(tmp);

            return true;
        }

    case CAST_UNSIGNED:
        {
            // cast to unsigned
            reil_inst->op = I_OR;
            reil_inst->b.type = A_CONST;
            reil_inst->b.size = reil_inst->c.size;
            reil_inst->b.val = 0;

            return true;
        }

    case CAST_SIGNED:
        {
            // cast to signed
            reil_size_t size_src = reil_inst->a.size;
            reil_size_t size_dst = reil_inst->c.size;

            reil_assert(size_dst > size_src, "invalid signed cast");
            
            Exp *tmp_0 = temp_operand(convert_operand_size(reil_inst->a.size), reil_inst->inum);            

            // get sign bit of the source value
            // AND src, mask, tmp_0
            NEW_INST(I_AND, reil_inst->inum);
            COPY_ARG(&new_inst.a, &reil_inst->a);
            new_inst.b.type = A_CONST;
            new_inst.b.size = new_inst.a.size;
            new_inst.b.val = reil_cast_mask_sign(new_inst.b.size);
            convert_operand(tmp_0, &new_inst.c);

            process_reil_inst(&new_inst);
            reil_inst->inum += 1;

            Exp *tmp_1 = temp_operand(REG_1, reil_inst->inum);

            // check if sign bit is zero
            // EQ tmp_0, 0, tmp_1
            NEW_INST(I_EQ, reil_inst->inum);
            convert_operand(tmp_0, &new_inst.a);
            new_inst.b.type = A_CONST;
            new_inst.b.size = new_inst.a.size;
            new_inst.b.val = 0;
            convert_operand(tmp_1, &new_inst.c);

            process_reil_inst(&new_inst);
            reil_inst->inum += 1;

            Exp *tmp_2 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);

            // extend value size
            // OR tmp_1, 0, tmp_2
            NEW_INST(I_OR, reil_inst->inum);
            convert_operand(tmp_1, &new_inst.a);
            new_inst.b.type = A_CONST;
            new_inst.b.size = size_dst;
            new_inst.b.val = 0;
            convert_operand(tmp_2, &new_inst.c);

            process_reil_inst(&new_inst);
            reil_inst->inum += 1;

            Exp *tmp_3 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);

            // set all bits if sign bit of source value was set
            // SUB tmp_2, 1, tmp_3
            NEW_INST(I_SUB, reil_inst->inum);
            convert_operand(tmp_2, &new_inst.a);
            new_inst.b.type = A_CONST;
            new_inst.b.size = size_dst;
            new_inst.b.val = 1;
            convert_operand(tmp_3, &new_inst.c);

            process_reil_inst(&new_inst);
            reil_inst->inum += 1;

            Exp *tmp_4 = temp_operand(convert_operand_size(size_dst), reil_inst->inum);

            // clear lower bits of the result
            // AND tmp_3, mask, tmp_4
            NEW_INST(I_AND, reil_inst->inum);
            convert_operand(tmp_3, &new_inst.a);
            new_inst.b.type = A_CONST;
            new_inst.b.size = size_dst;
            new_inst.b.val = reil_cast_mask(size_dst) & ~reil_cast_mask(size_src);
            convert_operand(tmp_4, &new_inst.c);

            process_reil_inst(&new_inst);
            reil_inst->inum += 1;

            // join result with the source value
            // OR src, tmp_4, dst
            reil_inst->op = I_OR;
            convert_operand(tmp_4, &reil_inst->b);

            free_bil_exp(tmp_0);
            free_bil_exp(tmp_1);
            free_bil_exp(tmp_2);
            free_bil_exp(tmp_3);
            free_bil_exp(tmp_4);

            return true;
        }    
    }

    return false;
}

Exp *CReilFromBilTranslator::process_bil_inst(reil_op_t inst, uint64_t inst_flags, Exp *c, Exp *exp)
{
    reil_inst_t reil_inst;
    Exp *a = NULL, *b = NULL;
    Exp *a_temp = NULL, *b_temp = NULL, *exp_temp = NULL;

    reil_assert(exp, "invalid expression");
    reil_assert(inst == I_STR || inst == I_JCC, "invalid instruction");
    
    memset(&reil_inst, 0, sizeof(reil_inst));
    reil_inst.op = inst;
    reil_inst.raw_info.addr = current_raw_info->addr;
    reil_inst.raw_info.size = current_raw_info->size;
    reil_inst.flags = inst_flags;

    if (c && c->exp_type == MEM)
    {
        // check for the store to memory
        reil_assert(reil_inst.op == I_STR, "invalid instruction used with memory operand");

        Mem *mem = (Mem *)c;    
        reil_inst.op = I_STM;

        // parse address expression
        Exp *addr = process_bil_exp(mem->addr);
        if (addr)
        {
            c = addr;
        }
        else
        {
            c = mem->addr;
        }

        // parse value expression
        if (exp_temp = process_bil_exp(exp))
        {
            exp = exp_temp;
        }
    }

    if (reil_inst.op == I_STR) 
    {
        reil_assert(c == NULL || c->exp_type == TEMP, 
            "invalid I_STR argument");
    }

    if (reil_inst.op == I_STM) 
    {
        reil_assert(c == NULL || (c->exp_type == TEMP || c->exp_type == CONSTANT),
            "invalid I_STM argument");
    }

    bool binary_logic = false;
    bool is_arshift = false, is_neq = false, is_le = false;
    
    // get a and b operands values from expression
    if (exp->exp_type == BINOP)
    {
        reil_assert(reil_inst.op == I_STR, "invalid instruction used with BINOP expression");

        // store result of binary operation
        BinOp *binop = (BinOp *)exp;  
        reil_inst.op = reil_inst_map_binop[binop->binop_type];

        if (binop->binop_type == LOGICAND || binop->binop_type == LOGICOR)
        {
            binary_logic = true;
        }

        if (binop->binop_type == ARSHIFT)
        {
            is_arshift = true;
        }
        else if (binop->binop_type == NEQ)
        {
            is_neq = true;
        }
        else if (binop->binop_type == LE)
        {
            is_le = true;
        }
        else
        {
            reil_assert(reil_inst.op != I_NONE, "invalid binop expression");
        }        

        a = binop->lhs;
        b = binop->rhs;
    }
    else if (exp->exp_type == UNOP)
    {
        reil_assert(reil_inst.op == I_STR, "invalid instruction used with UNOP expression");

        // store result of unary operation
        UnOp *unop = (UnOp *)exp;   
        reil_inst.op = reil_inst_map_unop[unop->unop_type];

        reil_assert(reil_inst.op != I_NONE, "invaid unop expression");

        a = unop->exp;
    }    
    else if (exp->exp_type == CAST)
    {
        reil_assert(reil_inst.op == I_STR, "invaid instruction used with CAST expression");

        // store with type cast
        Cast *cast = (Cast *)exp;

        a = cast->exp;     
    }
    else if (exp->exp_type == MEM)
    {
        reil_assert(reil_inst.op == I_STR, "invalid instruction used with memory operand");

        // read from memory and store
        Mem *mem = (Mem *)exp;
        reil_inst.op = I_LDM;

        if (a_temp = process_bil_exp(mem->addr))
        {
            a = a_temp;
        }
        else
        {
            a = mem->addr;
        }
    }     
    else if (exp->exp_type == TEMP || exp->exp_type == CONSTANT)
    {
        // store constant or register
        a = exp;
    }        
    else
    {
        reil_assert(0, "invalid expression");
    }

    // parse operand a expression
    if (a && (a_temp = process_bil_exp(a)))
    {
        a = a_temp;
    }   

    // parse operand b expression
    if (b && (b_temp = process_bil_exp(b)))
    {
        b = b_temp;
    }    

    reil_assert(a, "invalid instruction argument");
    
    reil_assert(a == NULL || a->exp_type == TEMP || a->exp_type == CONSTANT, 
        "invalid instruction argument");

    reil_assert(b == NULL || b->exp_type == TEMP || b->exp_type == CONSTANT, 
        "invalid instruction argument");    

    if (binary_logic)
    {
        // check for LOGICAND/LOGICOR size
        reil_assert(a == NULL || ((Temp *)a)->typ == REG_1, "invalid logic operand");
        reil_assert(b == NULL || ((Temp *)b)->typ == REG_1, "invalid logic operand");
    }

    if (c == NULL)
    {
        // allocate temporary value to store result
        reg_t tempreg_type;
        string tempreg_name;

        // determinate type for new value by type of result
        if (exp->exp_type == CAST)
        {
            Cast *cast = (Cast *)exp;
            tempreg_type = cast->typ;
        }
        else if (a->exp_type == TEMP)
        {
            Temp *temp = (Temp *)a;
            tempreg_type = temp->typ;
        }
        else if (a->exp_type == CONSTANT)
        {
            Constant *constant = (Constant *)a;
            tempreg_type = constant->typ;
        }
        else
        {
            reil_assert(0, "invaid expression");
        }        
        
        tempreg_name = tempreg_get_name(tempreg_alloc());
        c = new Temp(tempreg_type, tempreg_name);
    }            

    // make REIL operands from BIL expressions
    convert_operand(a, &reil_inst.a);
    convert_operand(b, &reil_inst.b);
    convert_operand(c, &reil_inst.c);

    reil_inst.inum = inst_count;
    inst_count += 1;

    if (exp->exp_type == CAST)
    {
        // generate code for BAP casts
        if (!process_bil_cast(exp, &reil_inst))
        {
            reil_assert(0, "process_bil_cast() fails");
        }
    }    

    if (is_arshift)
    {
        // generate code for BAP ARSHIFT
        process_bil_arshift(&reil_inst);
    }
    else if (is_neq)
    {
        // generate code for BAP NEQ
        process_bil_neq(&reil_inst);
    }
    else if (is_le)
    {
        // generate code for BAP LE
        process_bil_le(&reil_inst);   
    }

    // add assembled REIL instruction
    process_reil_inst(&reil_inst);

    free_bil_exp(a_temp);
    free_bil_exp(b_temp);
    free_bil_exp(exp_temp);

    return c;
}

void CReilFromBilTranslator::check_cjmp_false_target(Exp *target)
{
    if (target->exp_type != NAME)
    {
        reil_assert(0, "check_cjmp_false_target(): unexpected expression");
    }
    
    Stmt *s = get_bil_stmt(current_stmt + 1);
    if (s->stmt_type != LABEL)
    {
        reil_assert(0, "check_cjmp_false_target(): unexpected next statement type");   
    }

    Name *name = (Name *)target;
    Label *label = (Label *)s;

    // match next label name with the cjmp target name
    if (label->label != name->name)
    {
        reil_assert(0, "check_cjmp_false_target(): unexpected label");   
    }
}

void CReilFromBilTranslator::process_bil_stmt(Stmt *s, uint64_t inst_flags)
{
    switch (s->stmt_type)
    {
    case LABEL:
        {
            // label statement
            Label *label = (Label *)s;
            reil_addr_t label_addr = current_raw_info->addr;
            reil_inum_t label_inum = inst_count;

            if (inst_flags & IOPT_ASM_END)
            {
                // label belongs to the next instruction
                label_addr = current_raw_info->addr + current_raw_info->size;
                label_inum = 0;
            }

#ifdef DBG_BAP

            printf("// BAP label %s at 0x%llx.%.2x\n", label->label.c_str(), 
                label_addr, label_inum);
#endif
            break;
        }

    case MOVE:    
        {
            // move statement
            Move *move = (Move *)s;
            process_bil_inst(I_STR, inst_flags, move->lhs, move->rhs);
            break;    
        }       
    
    case JMP:
        {
            if (!(inst_flags & IOPT_CALL))
            {
                inst_flags |= IOPT_BB_END;
            }

            // jump statement
            Jmp *jmp = (Jmp *)s;
            Exp *target = jmp->target, *target_tmp = NULL;            

            if (target->exp_type == NAME)
            {
                Name *name = (Name *)target;
                reil_addr_t addr = 0;

                // find jump destination address by label name
                if (!get_bil_label(name->name, &addr))
                {
                    reil_assert(0, "get_bil_label() fails");
                }

                target = target_tmp = new Constant(REG_32, addr);
            }

            Constant cond(REG_1, 1);
            process_bil_inst(I_JCC, inst_flags, target, &cond);

            if (target_tmp)
            {
                free_bil_exp(target_tmp);
            }

            break;
        }

    case CJMP:
        {            
            // conditional jump statement
            CJmp *cjmp = (CJmp *)s;
            Exp *target = cjmp->t_target, *target_tmp = NULL;            
            Exp *cond = cjmp->cond, *cond_tmp = NULL;            

            if (target->exp_type == NAME)
            {
                Name *name = (Name *)target;
                reil_addr_t addr = 0;

                // find true target destination address by label name
                if (!get_bil_label(name->name, &addr))
                {
                    reil_assert(0, "get_bil_label() fails");
                }

                target = target_tmp = new Constant(REG_32, addr);
            }
          
            if (cond->exp_type != TEMP)
            {
                Exp *tmp = temp_operand(REG_1, inst_count);
                cond = cond_tmp = process_bil_inst(I_STR, 0, tmp, cond);
            }

            // verify that false target points to the next BAP instruction
            check_cjmp_false_target(cjmp->f_target);

            process_bil_inst(I_JCC, inst_flags | IOPT_BB_END, target, cond);

            if (target_tmp)
            {
                free_bil_exp(target_tmp);
            }

            if (cond_tmp)
            {
                free_bil_exp(cond_tmp);
            }

            break;
        }

    case CALL:
    case RETURN:
        {            
            fprintf(stderr, "ERROR: Statement %d is not implemented\n", s->stmt_type);
            reil_assert(0, "unimplemented statement");
        }

    case EXPSTMT:
    case COMMENT:
    case SPECIAL:
    case VARDECL:

        break;
    }  
}

bool CReilFromBilTranslator::is_unknown_insn(bap_block_t *block)
{   
    for (int i = 0; i < block->bap_ir->size(); i++)
    {
        // enumerate BIL statements        
        Stmt *s = block->bap_ir->at(i);

        if (s->stmt_type == SPECIAL)
        {
            // get special statement
            Special *special = (Special *)s;

            // check for unknown instruction tag
            if (special->special.find(uTag) == 0)
            {
                return true;
            }
        }
    }

    return false;
}

void CReilFromBilTranslator::process_empty_insn(void)
{
    reil_inst_t reil_inst;
    memset(&reil_inst, 0, sizeof(reil_inst));                

    reil_inst.op = I_NONE;
    reil_inst.raw_info.addr = current_raw_info->addr;
    reil_inst.raw_info.size = current_raw_info->size;

    reil_inst.flags = IOPT_ASM_END;
    process_reil_inst(&reil_inst);
}

void CReilFromBilTranslator::process_unknown_insn(void)
{  
    vector<Temp *>::iterator it;
    vector<Temp *> arg_src, arg_dst, arg_all;    

    // get instruction arguments
    disasm_arg_src(guest, current_raw_info->data, arg_src);
    disasm_arg_dst(guest, current_raw_info->data, arg_dst);   

    if (arg_src.size() > 0)
    {

#ifdef DBG_BAP

        printf("// src registers: ");
#endif
        for (it = arg_src.begin(); it != arg_src.end(); ++it)
        {

#ifdef DBG_BAP

            Temp *temp = *it;
            printf("%s ", temp->name.c_str());
#endif
            arg_all.push_back(*it);
        }

#ifdef DBG_BAP

        printf("\n");
#endif

    }    

    if (arg_dst.size() > 0)
    {

#ifdef DBG_BAP

        printf("// dst registers: ");
#endif
        for (it = arg_dst.begin(); it != arg_dst.end(); ++it)
        {

#ifdef DBG_BAP

            Temp *temp = *it;
            printf("%s ", temp->name.c_str());
#endif
            arg_all.push_back(*it);
        }

#ifdef DBG_BAP

        printf("\n");
#endif

    }

    reil_inst_t reil_inst;
    memset(&reil_inst, 0, sizeof(reil_inst));                

    reil_inst.op = I_UNK;
    reil_inst.raw_info.addr = current_raw_info->addr;
    reil_inst.raw_info.size = current_raw_info->size;

    if (arg_all.size() == 0)
    {
        // no arguments found
        reil_inst.flags = IOPT_ASM_END;
        process_reil_inst(&reil_inst);
    }

    for (it = arg_all.begin(); it != arg_all.end(); ++it)
    {
        Temp *temp = *it;        

        if (it + 1 == arg_all.end())
        {
            reil_inst.flags = IOPT_ASM_END;        
        }        

        if (find(arg_src.begin(), arg_src.end(), temp) != arg_src.end())
        {
            // generate I_UNK with source argument access
            memset(&reil_inst.c, 0, sizeof(reil_arg_t));
            convert_operand(temp, &reil_inst.a);
            process_reil_inst(&reil_inst);
        }
        else if (find(arg_dst.begin(), arg_dst.end(), temp) != arg_dst.end())
        {
            // generate I_UNK with destination argument access
            memset(&reil_inst.a, 0, sizeof(reil_arg_t));
            convert_operand(temp, &reil_inst.c);
            process_reil_inst(&reil_inst);
        }

        reil_inst.inum += 1;
    }   

    for (it = arg_all.begin(); it != arg_all.end(); ++it)
    {
        Temp *temp = *it;        
        delete temp;
    }
}

bool CReilFromBilTranslator::get_bil_label(string name, reil_addr_t *addr)
{
    reil_addr_t ret = 0;
    const char *c_name = name.c_str();

    if (!strncmp(c_name, "pc_0x", 5))
    {
        // get code pointer from label text
        ret = strtoll(c_name + 5, NULL, 16);
        reil_assert(errno != EINVAL, "invalid pc value");

        if (addr)
        {
            *addr = ret;
        }

        return true;
    }

    if (current_block == NULL)
    {
        reil_assert(0, "get_bil_label(): invalid BAP block");
    }
    
    int size = current_block->bap_ir->size();
    
    // lookup for statement with the given label
    for (int i = 0; i < size; i++)
    {
        // enumerate BIL statements        
        Stmt *s = current_block->bap_ir->at(i);
        uint64_t inst_flags = IOPT_ASM_END;    

        for (int n = i + 1; n < size; n++)
        {
            // check for last IR instruction
            Stmt *s_next = current_block->bap_ir->at(n);

            if (s_next->stmt_type == MOVE || 
                s_next->stmt_type == CJMP ||
                s_next->stmt_type == JMP)
            {
                inst_flags = 0;
                break;
            }            
        }

        switch (s->stmt_type)
        {
        case LABEL:
            {
                Label *label = (Label *)s;

                // find label by name
                if (label->label == name)
                {
                    if (inst_flags & IOPT_ASM_END)
                    {
                        // label belongs to the next instruction
                        ret = current_raw_info->addr + current_raw_info->size;
#ifdef DBG_BAP
                        printf("// %s -> 0x%llx\n", name.c_str(), ret);
#endif
                    }
                    else
                    {
                        reil_assert(0, "labels at the middle of the BAP instruction are not implemented");
                    }

                    if (addr)
                    {
                        *addr = ret;
                    }

                    return true;
                }                

                break;
            }
        }        
    }     

    return false;
}

Stmt *CReilFromBilTranslator::get_bil_stmt(int pos)
{
    if (current_block == NULL)
    {
        reil_assert(0, "get_bil_stmt(): invalid BAP block");
    }

    int size = current_block->bap_ir->size();
    if (pos >= size)
    {
        reil_assert(0, "get_bil_stmt(): invalid BAP statement position");
    }

    return current_block->bap_ir->at(pos);
}

void CReilFromBilTranslator::process_bil(reil_raw_t *raw_info, bap_block_t *block)
{
    int size = block->bap_ir->size();

    reset_state(block);

    if (raw_info)
    {
        current_raw_info = raw_info;
    }

    if (is_unknown_insn(block))
    {
        fprintf(stderr, "WARNING: 0x%llx was not translated\n", raw_info->addr);

        // add metainformation about unknown instruction into the code
        process_unknown_insn();

        goto _end;
    }
    
    for (int i = 0; i < size; i++)
    {
        current_stmt = i;

        // enumerate BIL statements        
        Stmt *s = block->bap_ir->at(i);
        uint64_t inst_flags = IOPT_ASM_END;    

        for (int n = i + 1; n < size; n++)
        {
            // check for last IR instruction
            Stmt *s_next = block->bap_ir->at(n);

            if (s_next->stmt_type == MOVE || 
                s_next->stmt_type == CJMP ||
                s_next->stmt_type == JMP)
            {
                inst_flags = 0;
                break;
            }            
        }

        if (i < size - 1)
        {
            // check for the special statement that following current
            Stmt *s_next = block->bap_ir->at(i + 1);
            if (s_next->stmt_type == SPECIAL)
            {
                Special *special = (Special *)s_next;
                
                // translate special statement to the REIL instruction options
                inst_flags |= convert_special(special);
            }
        }   

#ifdef DBG_BAP
        
        printf("%s\n", s->tostring().c_str());
#endif     

        // convert statement to REIL code
        process_bil_stmt(s, inst_flags);
    }

    if (inst_count == 0)
    {
        // add I_NONE
        process_empty_insn();
    }

_end:

#ifdef DBG_BAP
        
    printf("\n");

#endif  

    return;
}

CReilTranslator::CReilTranslator(VexArch arch, reil_inst_handler_t handler, void *context)
{
    // initialize libasmir
    translate_init();

    guest = arch;
    translator = new CReilFromBilTranslator(arch, handler, context);
    assert(translator);
}

CReilTranslator::~CReilTranslator()
{
    delete translator;
}

int CReilTranslator::process_inst(address_t addr, uint8_t *data, int size)
{
    int ret = 0;
    reil_raw_t raw_info;
    memset(&raw_info, 0, sizeof(raw_info));
    
    // translate to VEX
    bap_block_t *block = generate_vex_ir(guest, data, addr);
    
    assert(block);
    assert(block->inst_size != 0 && block->inst_size != -1);

    ret = block->inst_size;

    // tarnslate to BAP
    generate_bap_ir_block(guest, block);  

#ifdef DBG_BAP

    printf(
        "// %.8llx: %s %s ; len = %d\n",
        addr, block->str_mnem.c_str(), block->str_op.c_str(), 
        block->inst_size
    );              
    
#endif

    raw_info.addr = addr;
    raw_info.size = ret;
    raw_info.data = data;

    // cast to char* is needed for successful work with cython
    raw_info.str_mnem = (char *)block->str_mnem.c_str();
    raw_info.str_op = (char *)block->str_op.c_str();

    // generate REIL
    translator->process_bil(&raw_info, block);

    for (int i = 0; i < block->bap_ir->size(); i++)
    {
        // free BIL code
        Stmt *s = block->bap_ir->at(i);
        Stmt::destroy(s);
    }

    delete block->bap_ir;
    delete block;        
    
    // free VEX memory
    // asmir_close() is also doing that
    vx_FreeAll();
    
    return ret;
}
