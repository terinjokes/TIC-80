#include "core/core.h"

#if defined(TIC_BUILD_WITH_RUBY)

#include "tools.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <mruby.h>
#include <mruby/compile.h>
#include <mruby/string.h>
#include <mruby/array.h>

static const char* const RubyKeywords [] =
  {
    "BEGIN", "END", "alias", "and", "begin", "break",
    "case", "class", "def", "defined?", "do", "else",
    "elsif", "end", "ensure", "false", "for", "if",
    "in", "module", "next", "nil", "not", "or", "redo",
    "rescure", "retry", "return", "self", "super", "then",
    "true", "undef", "unless", "until", "when", "while",
    "yield",
  };

mrb_value ruby_cls(mrb_state* mrb, mrb_value self) {
  tic_mem* tic = (tic_mem*)mrb->ud;
  mrb_int color;

  mrb_get_args(mrb, "i", &color);

  tic_api_cls(tic, (int32_t)color);

  return self;
}

mrb_value ruby_spr(mrb_state* mrb, mrb_value self) {
  static u8 colors[TIC_PALETTE_SIZE];
  s32 count = 0;
  tic_mem* tic = (tic_mem*)mrb->ud;
  mrb_int index, x, y;
  mrb_int w = 1, h = 1, scale = 1, flip = 0, rotate = 0;

  uint32_t kw_req = 0;
  mrb_sym kw_names[6] = {
    mrb_intern_cstr(mrb, "w"),
    mrb_intern_cstr(mrb, "h"),
    mrb_intern_cstr(mrb, "scale"),
    mrb_intern_cstr(mrb, "flip"),
    mrb_intern_cstr(mrb, "rotate"),
    mrb_intern_cstr(mrb, "colorkey"),
  };
  mrb_value kw_values[6];
  const mrb_kwargs kwargs = { 6, kw_req, kw_names, kw_values, NULL };

  mrb_get_args(mrb, "iii:", &index, &x, &y, &kwargs);

  if (!mrb_undef_p(kw_values[0])) w = mrb_int(mrb, kw_values[0]);
  if (!mrb_undef_p(kw_values[1])) h = mrb_int(mrb, kw_values[1]);
  if (!mrb_undef_p(kw_values[2])) scale = mrb_int(mrb, kw_values[2]);
  if (!mrb_undef_p(kw_values[3])) flip = mrb_int(mrb, kw_values[3]);
  if (!mrb_undef_p(kw_values[4])) rotate = mrb_int(mrb, kw_values[4]);

  if (!mrb_undef_p(kw_values[5])) {
    if (mrb_array_p(kw_values[5])) {
      for (s32 i = 0; i < TIC_PALETTE_SIZE; i++) {
        mrb_value color = mrb_ary_ref(mrb, kw_values[5], i);
        if (!mrb_nil_p(color)) {
          colors[i] = mrb_int(mrb, color);
          count++;
        }
      }
    }
    else {
      colors[0] = mrb_int(mrb, kw_values[5]);
      count++;
    }
  }

  tic_api_spr(tic, (s32)index, (s32)x, (s32)y, (s32)w, (s32)h, colors, count, (s32)scale, (s32)flip, (s32)rotate);

  return self;
}

static void closeRuby(tic_mem* tic) {
  tic_core* core = (tic_core*)tic;

  if (core->ruby) {
    mrb_close(core->ruby);
    core->ruby = NULL;
  }
}

static void initCore(tic_core* core) {
  mrb_state *mrb = core->ruby;
  mrb->ud = core;

  mrb_define_method(mrb, mrb->kernel_module, "cls", ruby_cls, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, mrb->kernel_module, "spr", ruby_spr, MRB_ARGS_REQ(3));
}

static bool initRuby(tic_mem* tic, const char* code) {
  tic_core* core = (tic_core*)tic;

  closeRuby(tic);

  mrb_state *mrb = core->ruby = mrb_open();
  if (!mrb) {
    return false;
  }

  initCore(core);

  mrb_load_string(mrb, code);
  if (mrb->exc) {
    mrb_value v = mrb_obj_value(mrb->exc);
    core->data->error(core->data->data, mrb_str_to_cstr(mrb, v));
    return false;
  }

  return true;
}

static void callRubyTick(tic_mem* tic) {
  tic_core* core = (tic_core*)tic;
  mrb_state *mrb = core->ruby;

  mrb_funcall(mrb, mrb_top_self(mrb), TIC_FN, 0);
}

static void callRubyScanline(tic_mem* tic, s32 row, void* data) {
  tic_core* core = (tic_core*)tic;
  mrb_state *mrb = core->ruby;

  mrb_bool resp_p = mrb_obj_respond_to(mrb, mrb->kernel_module, mrb_intern_cstr(mrb, SCN_FN));
  if (resp_p == 1) {
    mrb_funcall(mrb, mrb_top_self(mrb), SCN_FN, 1, row);
    if (mrb->exc) {
      mrb_value v = mrb_obj_value(mrb->exc);
      core->data->error(core->data->data, mrb_str_to_cstr(mrb, v));
    }
  }
}

static void callRubyOverline(tic_mem* tic, void* data) {
  tic_core* core = (tic_core*)tic;
  mrb_state *mrb = core->ruby;

  mrb_bool resp_p = mrb_obj_respond_to(mrb, mrb->kernel_module, mrb_intern_cstr(mrb, OVR_FN));
  if (resp_p == 1) {
    mrb_funcall(mrb, mrb_top_self(mrb), OVR_FN, 0);
    if (mrb->exc) {
      mrb_value v = mrb_obj_value(mrb->exc);
      core->data->error(core->data->data, mrb_str_to_cstr(mrb, v));
    }
  }
}

static const tic_outline_item* getRubyOutline(const char* code, s32* size) {
  enum{Size = sizeof(tic_outline_item)};

  *size = 0;

  static tic_outline_item* items = NULL;

  if (!items) {
    free(items);
    items = NULL;
  }

  const char* ptr = code;

  while(true) {
    static const char FuncString[] = "def ";

    ptr = strstr(ptr, FuncString);

    if (ptr) {
      ptr += sizeof FuncString - 1;

      const char* start = ptr;
      const char* end = start;

      while(*ptr) {
        char c = *ptr;

        if (!isalnum(c)) {
          end = ptr;
          break;
        }

        ptr++;
      }

      if (end > start) {
        items = realloc(items, (*size + 1) * Size);

        items[*size].pos = start;
        items[*size].size = (s32)(end - start);

        (*size)++;
      }
    }
    else break;
  }

  return items;
}

void evalRuby(tic_mem* tic, const char* code) {
  tic_core* core = (tic_core*)tic;
  mrb_state *mrb = core->ruby;

  if (!mrb) return;

  mrb_load_string(mrb, code);
  if (mrb->exc) {
    mrb_value v = mrb_obj_value(mrb->exc);
    core->data->error(core->data->data, mrb_str_to_cstr(mrb, v));
  }
}

static const tic_script_config RubySyntaxConfig =
  {
    .init = initRuby,
    .close = closeRuby,
    .tick = callRubyTick,
    .scanline = callRubyScanline,
    .overline = callRubyOverline,

    .getOutline = getRubyOutline,
    .eval = evalRuby,

    .blockCommentStart = "==begin",
    .blockCommentEnd = "==end",
    .blockCommentStart2 = NULL,
    .blockCommentEnd2 = NULL,
    .blockStringStart = NULL,
    .blockStringEnd = NULL,
    .singleComment = "#",

    .keywords = RubyKeywords,
    .keywordsCount = COUNT_OF(RubyKeywords),
  };

const tic_script_config* getRubyConfig() {
  return &RubySyntaxConfig;
}

#endif
