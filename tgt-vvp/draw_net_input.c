/*
 * Copyright (c) 2001-2010 Stephen Williams (steve@icarus.com)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

# include  "vvp_priv.h"
# include  <stdlib.h>
# include  <math.h>
# include  <string.h>
# include  <inttypes.h>
# include  <assert.h>

#ifdef __MINGW32__  /* MinGW has inconsistent %p output. */
#define snprintf _snprintf
#endif

static ivl_signal_type_t signal_type_of_nexus(ivl_nexus_t nex)
{
      unsigned idx;
      ivl_signal_type_t out = IVL_SIT_TRI;

      for (idx = 0 ;  idx < ivl_nexus_ptrs(nex) ;  idx += 1) {
	    ivl_signal_type_t stype;
	    ivl_nexus_ptr_t ptr = ivl_nexus_ptr(nex, idx);
	    ivl_signal_t sig = ivl_nexus_ptr_sig(ptr);
	    if (sig == 0)
		  continue;

	    stype = ivl_signal_type(sig);
	    if (stype == IVL_SIT_REG)
		  continue;
	    if (stype == IVL_SIT_TRI)
		  continue;
	    if (stype == IVL_SIT_NONE)
		  continue;
	    if (stype == IVL_SIT_UWIRE) return IVL_SIT_UWIRE;
	    out = stype;
      }

      return out;
}

static ivl_variable_type_t signal_data_type_of_nexus(ivl_nexus_t nex)
{
      unsigned idx;

      for (idx = 0 ;  idx < ivl_nexus_ptrs(nex) ;  idx += 1) {
	    ivl_nexus_ptr_t ptr = ivl_nexus_ptr(nex, idx);
	    ivl_signal_t sig = ivl_nexus_ptr_sig(ptr);
	    if (sig == 0) continue;

	    return ivl_signal_data_type(sig);
      }

      return IVL_VT_NO_TYPE;
}

static void draw_C4_repeated_constant(char bit_char, unsigned width)
{
      unsigned idx;

      fprintf(vvp_out, "C4<");
      for (idx = 0 ;  idx < width ;  idx += 1)
	    fprintf(vvp_out, "%c", bit_char);

      fprintf(vvp_out, ">");
}

static char* draw_C4_to_string(ivl_net_const_t cptr)
{
      const char*bits = ivl_const_bits(cptr);
      unsigned idx;

      size_t result_len = 5 + ivl_const_width(cptr);
      char*result = malloc(result_len);
      char*dp = result;
      strcpy(dp, "C4<");
      dp += strlen(dp);

      for (idx = 0 ;  idx < ivl_const_width(cptr) ;  idx += 1) {
	    char bitchar = bits[ivl_const_width(cptr)-idx-1];
	    *dp++ = bitchar;
	    assert(dp >= result);
	    assert((unsigned)(dp - result) < result_len);
      }

      strcpy(dp, ">");
      return result;
}

static char* draw_C8_to_string(ivl_net_const_t cptr,
			       ivl_drive_t dr0, ivl_drive_t dr1)
{
      size_t nresult = 5 + 3*ivl_const_width(cptr);
      char*result = malloc(nresult);
      const char*bits = ivl_const_bits(cptr);
      unsigned idx;

      char dr0c = "01234567"[dr0];
      char dr1c = "01234567"[dr1];
      char*dp = result;

      strcpy(dp, "C8<");
      dp += strlen(dp);

      for (idx = 0 ;  idx < ivl_const_width(cptr) ;  idx += 1) {
	    switch (bits[ivl_const_width(cptr)-idx-1]) {
		case '0':
		  *dp++ = dr0c;
		  *dp++ = dr0c;
		  *dp++ = '0';
		  break;
		case '1':
		  *dp++ = dr1c;
		  *dp++ = dr1c;
		  *dp++ = '1';
		  break;
		case 'x':
		case 'X':
		  *dp++ = dr0c;
		  *dp++ = dr1c;
		  *dp++ = 'x';
		  break;
		case 'z':
		case 'Z':
		  *dp++ = '0';
		  *dp++ = '0';
		  *dp++ = 'z';
		  break;
		default:
		  assert(0);
		  break;
	    }
	    assert(dp >= result);
	    assert((unsigned)(dp - result) < nresult);
      }

      strcpy(dp, ">");
      return result;
}

static struct vvp_nexus_data*new_nexus_data()
{
      struct vvp_nexus_data*data = calloc(1, sizeof(struct vvp_nexus_data));
      return data;
}

static int nexus_drive_is_strength_aware(ivl_nexus_ptr_t nptr)
{
      ivl_net_logic_t logic;

      if (ivl_nexus_ptr_drive0(nptr) != IVL_DR_STRONG)
	    return 1;
      if (ivl_nexus_ptr_drive1(nptr) != IVL_DR_STRONG)
	    return 1;

      logic = ivl_nexus_ptr_log(nptr);
      if (logic != 0) {
	      /* These logic gates are able to generate unusual
	         strength values and so their outputs are considered
	         strength aware. */
	    if (ivl_logic_type(logic) == IVL_LO_BUFIF0)
		  return 1;
	    if (ivl_logic_type(logic) == IVL_LO_BUFIF1)
		  return 1;
	    if (ivl_logic_type(logic) == IVL_LO_PMOS)
		  return 1;
	    if (ivl_logic_type(logic) == IVL_LO_NMOS)
		  return 1;
	    if (ivl_logic_type(logic) == IVL_LO_CMOS)
		  return 1;
      }

      return 0;
}

/*
 * Given a nexus, look for a signal that has module delay
 * paths. Return that signal. (There should be no more than 1.) If we
 * don't find any, then return nil.
 */
static ivl_signal_t find_modpath(ivl_nexus_t nex)
{
      unsigned idx;
      for (idx = 0 ;  idx < ivl_nexus_ptrs(nex) ;  idx += 1) {
	    ivl_nexus_ptr_t ptr = ivl_nexus_ptr(nex,idx);
	    ivl_signal_t sig = ivl_nexus_ptr_sig(ptr);
	    if (sig == 0)
		  continue;
	    if (ivl_signal_npath(sig) == 0)
		  continue;

	    return sig;
      }

      return 0;
}

static void str_repeat(char*buf, const char*str, unsigned rpt)
{
      unsigned idx;
      size_t len = strlen(str);
      for (idx = 0 ;  idx < rpt ;  idx += 1) {
	    strcpy(buf, str);
	    buf += len;
      }
}

/*
 * This function takes a nexus and looks for an input functor. It then
 * draws to the output a string that represents that functor. What we
 * are trying to do here is find the input to the net that is attached
 * to this nexus.
 */

static char* draw_net_input_drive(ivl_nexus_t nex, ivl_nexus_ptr_t nptr)
{
      unsigned nptr_pin = ivl_nexus_ptr_pin(nptr);
      ivl_net_const_t cptr;
      ivl_net_logic_t lptr;
      ivl_signal_t sptr;
      ivl_lpm_t lpm;

      lptr = ivl_nexus_ptr_log(nptr);
      if (lptr
	  && ((ivl_logic_type(lptr)==IVL_LO_BUFZ)||(ivl_logic_type(lptr)==IVL_LO_BUFT))
	  && (nptr_pin == 0))
	    do {
		  if (! can_elide_bufz(lptr, nptr))
			break;

		  return strdup(draw_net_input(ivl_logic_pin(lptr, 1)));
	    } while(0);

	/* If this is a pulldown device, then there is a single pin
	   that drives a constant value to the entire width of the
	   vector. The driver normally drives a pull0 value, so a C8<>
	   constant is appropriate, but if the drive is really strong,
	   then we can draw a C4<> constant instead. */
      if (lptr && (ivl_logic_type(lptr) == IVL_LO_PULLDOWN)) {
	    if (ivl_nexus_ptr_drive0(nptr) == IVL_DR_STRONG) {
		  size_t result_len = ivl_logic_width(lptr) + 5;
		  char*result = malloc(result_len);
		  char*dp = result;
		  strcpy(dp, "C4<");
		  dp += strlen(dp);
		  str_repeat(dp, "0", ivl_logic_width(lptr));
		  dp += ivl_logic_width(lptr);
		  *dp++ = '>';
		  *dp = 0;
		  assert(dp >= result);
		  assert((unsigned)(dp - result) <= result_len);
		  return result;
	    } else {
		  char val[4];
		  size_t result_len = 3*ivl_logic_width(lptr) + 5;
		  char*result = malloc(result_len);
		  char*dp = result;

		  val[0] = "01234567"[ivl_nexus_ptr_drive0(nptr)];
		  val[1] = val[0];
		  val[2] = '0';
		  val[3] = 0;

		  strcpy(dp, "C8<");
		  dp += strlen(dp);
		  str_repeat(dp, val, ivl_logic_width(lptr));
		  dp += 3*ivl_logic_width(lptr);
		  *dp++ = '>';
		  *dp = 0;
		  assert(dp >= result);
		  assert((unsigned)(dp - result) <= result_len);
		  return result;
	    }
      }

      if (lptr && (ivl_logic_type(lptr) == IVL_LO_PULLUP)) {
	    char*result;
	    char tmp[32];
	    if (ivl_nexus_ptr_drive1(nptr) == IVL_DR_STRONG) {
		  size_t result_len = 5 + ivl_logic_width(lptr);
		  result = malloc(result_len);
		  char*dp = result;
		  strcpy(dp, "C4<");
		  dp += strlen(dp);
		  str_repeat(dp, "1", ivl_logic_width(lptr));
		  dp += ivl_logic_width(lptr);
		  *dp++ = '>';
		  *dp = 0;
		  assert(dp >= result);
		  assert((unsigned)(dp - result) <= result_len);

	    } else {
		  char val[4];
		  size_t result_len = 5 + 3*ivl_logic_width(lptr);
		  result = malloc(result_len);
		  char*dp = result;

		  val[0] = "01234567"[ivl_nexus_ptr_drive1(nptr)];
		  val[1] = val[0];
		  val[2] = '1';
		  val[3] = 0;

		  strcpy(dp, "C8<");
		  dp += strlen(dp);
		  str_repeat(dp, val, ivl_logic_width(lptr));
		  dp += 3*ivl_logic_width(lptr);
		  *dp++ = '>';
		  *dp = 0;
		  assert(dp >= result);
		  assert((unsigned)(dp - result) <= result_len);

	    }

	      /* Make the constant an argument to a BUFZ, which is
		 what we use to drive the PULLed value. */
	    fprintf(vvp_out, "L_%p .functor BUFT 1, %s, C4<0>, C4<0>, C4<0>;\n",
		    lptr, result);
	    snprintf(tmp, sizeof tmp, "L_%p", lptr);
	    result = realloc(result, strlen(tmp)+1);
	    strcpy(result, tmp);
	    return result;
      }

      if (lptr && (nptr_pin == 0)) {
	    char tmp[128];
	    snprintf(tmp, sizeof tmp, "L_%p", lptr);
	    return strdup(tmp);
      }

      sptr = ivl_nexus_ptr_sig(nptr);
      if (sptr && (ivl_signal_type(sptr) == IVL_SIT_REG)) {
	    char tmp[128];
	      /* Input is a .var. This device may be a non-zero pin
	         because it may be an array of reg vectors. */
	    snprintf(tmp, sizeof tmp, "v%p_%u", sptr, nptr_pin);

	    if (ivl_signal_dimensions(sptr) > 0) {
		  fprintf(vvp_out, "v%p_%u .array/port v%p, %u;\n",
			  sptr, nptr_pin, sptr, nptr_pin);
	    }

	    return strdup(tmp);
      }

      cptr = ivl_nexus_ptr_con(nptr);
      if (cptr) {
	    char *result = 0;
	    ivl_expr_t d_rise, d_fall, d_decay;
            unsigned dly_width = 0;

	      /* Constants should have exactly 1 pin, with a literal value. */
	    assert(nptr_pin == 0);

	    switch (ivl_const_type(cptr)) {
		case IVL_VT_LOGIC:
		case IVL_VT_BOOL:
		  if ((ivl_nexus_ptr_drive0(nptr) == IVL_DR_STRONG)
		      && (ivl_nexus_ptr_drive1(nptr) == IVL_DR_STRONG)) {

			result = draw_C4_to_string(cptr);

		  } else {
			result = draw_C8_to_string(cptr,
						   ivl_nexus_ptr_drive0(nptr),
						   ivl_nexus_ptr_drive1(nptr));
		  }
                  dly_width = ivl_const_width(cptr);
		  break;

		case IVL_VT_REAL:
		  result = draw_Cr_to_string(ivl_const_real(cptr));
                  dly_width = 0;
		  break;

		default:
		  assert(0);
		  break;
	    }

	    d_rise = ivl_const_delay(cptr, 0);
	    d_fall = ivl_const_delay(cptr, 1);
	    d_decay = ivl_const_delay(cptr, 2);

	      /* We have a delayed constant, so we need to build some code. */
	    if (d_rise != 0) {
		  char tmp[128];
		  fprintf(vvp_out, "L_%p/d .functor BUFT 1, %s, "
		                   "C4<0>, C4<0>, C4<0>;\n", cptr, result);
		  free(result);

		    /* Is this a fixed or variable delay? */
		  if (number_is_immediate(d_rise, 64, 0) &&
		      number_is_immediate(d_fall, 64, 0) &&
		      number_is_immediate(d_decay, 64, 0)) {

			assert(! number_is_unknown(d_rise));
			assert(! number_is_unknown(d_fall));
			assert(! number_is_unknown(d_decay));

			fprintf(vvp_out, "L_%p .delay %u "
				"(%" PRIu64 ",%" PRIu64 ",%" PRIu64 ") L_%p/d;\n",
			                 cptr, dly_width,
			                 get_number_immediate64(d_rise),
			                 get_number_immediate64(d_fall),
			                 get_number_immediate64(d_decay), cptr);

		  } else {
			ivl_signal_t sig;
			// We do not currently support calculating the decay
			// from the rise and fall variable delays.
			assert(d_decay != 0);
			assert(ivl_expr_type(d_rise) == IVL_EX_SIGNAL);
			assert(ivl_expr_type(d_fall) == IVL_EX_SIGNAL);
			assert(ivl_expr_type(d_decay) == IVL_EX_SIGNAL);

			fprintf(vvp_out, "L_%p .delay %u L_%p/d",
                                cptr, dly_width, cptr);

			sig = ivl_expr_signal(d_rise);
			assert(ivl_signal_dimensions(sig) == 0);
			fprintf(vvp_out, ", v%p_0", sig);

			sig = ivl_expr_signal(d_fall);
			assert(ivl_signal_dimensions(sig) == 0);
			fprintf(vvp_out, ", v%p_0", sig);

			sig = ivl_expr_signal(d_decay);
			assert(ivl_signal_dimensions(sig) == 0);
			fprintf(vvp_out, ", v%p_0;\n", sig);
		  }

		  snprintf(tmp, sizeof tmp, "L_%p", cptr);
		  result = strdup(tmp);

	    } else {
		  char tmp[64];
		  fprintf(vvp_out, "L_%p .functor BUFT 1, %s, "
			  "C4<0>, C4<0>, C4<0>;\n", cptr, result);
		  free(result);

		  snprintf(tmp, sizeof tmp, "L_%p", cptr);
		  result = strdup(tmp);
	    }

	    return result;
      }

      lpm = ivl_nexus_ptr_lpm(nptr);
      if (lpm) switch (ivl_lpm_type(lpm)) {

	  case IVL_LPM_FF:
	  case IVL_LPM_ABS:
	  case IVL_LPM_ADD:
	  case IVL_LPM_ARRAY:
	  case IVL_LPM_CAST_INT2:
	  case IVL_LPM_CAST_INT:
	  case IVL_LPM_CAST_REAL:
	  case IVL_LPM_CONCAT:
	  case IVL_LPM_CMP_EEQ:
	  case IVL_LPM_CMP_EQ:
	  case IVL_LPM_CMP_GE:
	  case IVL_LPM_CMP_GT:
	  case IVL_LPM_CMP_NE:
	  case IVL_LPM_CMP_NEE:
	  case IVL_LPM_RE_AND:
	  case IVL_LPM_RE_OR:
	  case IVL_LPM_RE_XOR:
	  case IVL_LPM_RE_NAND:
	  case IVL_LPM_RE_NOR:
	  case IVL_LPM_RE_XNOR:
	  case IVL_LPM_SFUNC:
	  case IVL_LPM_SHIFTL:
	  case IVL_LPM_SHIFTR:
	  case IVL_LPM_SIGN_EXT:
	  case IVL_LPM_SUB:
	  case IVL_LPM_MULT:
	  case IVL_LPM_MUX:
	  case IVL_LPM_POW:
	  case IVL_LPM_DIVIDE:
	  case IVL_LPM_MOD:
	  case IVL_LPM_UFUNC:
	  case IVL_LPM_PART_VP:
	  case IVL_LPM_PART_PV: /* NOTE: This is only a partial driver. */
	  case IVL_LPM_REPEAT:
	    if (ivl_lpm_q(lpm) == nex) {
		  char tmp[128];
		  snprintf(tmp, sizeof tmp, "L_%p", lpm);
		  return strdup(tmp);
	    }
	    break;

      }

      fprintf(stderr, "vvp.tgt error: no input to nexus.\n");
      assert(0);
      return strdup("C<z>");
}

static char* draw_island_port(ivl_island_t island, int island_input_flag,
			      ivl_nexus_t nex, struct vvp_nexus_data*nex_data,
			      const char*src)
{
      char result[64];
      if (ivl_island_flag_test(island,0) == 0) {
	    fprintf(vvp_out, "I%p .island tran;\n", island);
	    ivl_island_flag_set(island,0,1);
      }

      snprintf(result, sizeof result, "p%p", nex);
      assert(nex_data->island == 0);
      nex_data->island = island;
      assert(nex_data->island_input == 0);
      nex_data->island_input = strdup(result);

      if (island_input_flag) {
	    fprintf(vvp_out, "p%p .import I%p, %s;\n", nex, island, src);
	    return strdup(src);
      } else {
	    fprintf(vvp_out, "p%p .port I%p, %s;\n", nex, island, src);
	    return strdup(nex_data->island_input);
      }
}

/*
 * This function draws the input to a net into a string. What that
 * means is that it returns a static string that can be used to
 * represent a resolved driver to a nexus. If there are multiple
 * drivers to the nexus, then it writes out the resolver declarations
 * needed to perform strength resolution.
 *
 * The string that this returns is malloced, and that means that the
 * caller must free the string or store it permanently. This function
 * does *not* check for a previously calculated string. Use the
 * draw_net_input for the general case.
 */

static ivl_nexus_ptr_t *drivers = 0x0;
static unsigned adrivers = 0;

void EOC_cleanup_drivers()
{
      free(drivers);
      drivers = NULL;
      adrivers = 0;
}

static void draw_net_input_x(ivl_nexus_t nex,
			     struct vvp_nexus_data*nex_data)
{
      ivl_island_t island = 0;
      int island_input_flag = -1;
      ivl_signal_type_t res;
      char result[512];
      unsigned idx;
      int level;
      unsigned ndrivers = 0;

      const char*resolv_type;

      char*nex_private = 0;

	/* Accumulate nex_data flags. */
      int nex_flags = 0;

      res = signal_type_of_nexus(nex);
      switch (res) {
	  case IVL_SIT_TRI:
	  case IVL_SIT_UWIRE:
	    resolv_type = "tri";
	    break;
	  case IVL_SIT_TRI0:
	    resolv_type = "tri0";
	    nex_flags |= VVP_NEXUS_DATA_STR;
	    break;
	  case IVL_SIT_TRI1:
	    resolv_type = "tri1";
	    nex_flags |= VVP_NEXUS_DATA_STR;
	    break;
	  case IVL_SIT_TRIAND:
	    resolv_type = "triand";
	    break;
	  case IVL_SIT_TRIOR:
	    resolv_type = "trior";
	    break;
	  default:
	    fprintf(stderr, "vvp.tgt: Unsupported signal type: %u\n", res);
	    assert(0);
	    resolv_type = "tri";
	    break;
      }


      for (idx = 0 ;  idx < ivl_nexus_ptrs(nex) ;  idx += 1) {
	    ivl_switch_t sw = 0;
	    ivl_nexus_ptr_t nptr = ivl_nexus_ptr(nex, idx);

	      /* If this object is part of an island, then we'll be
	         making a port. If this nexus is an output from any
	         switches in the island, then set island_input_flag to
	         false. Save the island cookie. */
	    if ( (sw = ivl_nexus_ptr_switch(nptr)) ) {
		  assert(island == 0 || island == ivl_switch_island(sw));
		  island = ivl_switch_island(sw);
		  if (nex == ivl_switch_a(sw))
			island_input_flag = 0;
		  else if (nex == ivl_switch_b(sw))
			island_input_flag = 0;
		  else if (island_input_flag == -1) {
			assert(nex == ivl_switch_enable(sw));
			island_input_flag = 1;
		  }
	    }

	      /* Skip input only pins. */
	    if ((ivl_nexus_ptr_drive0(nptr) == IVL_DR_HiZ)
		&& (ivl_nexus_ptr_drive1(nptr) == IVL_DR_HiZ))
		  continue;

	      /* Mark the strength-aware flag if the driver can
		 generate values other than the standard "6"
		 strength. */
	    if (nexus_drive_is_strength_aware(nptr))
		  nex_flags |= VVP_NEXUS_DATA_STR;

	      /* Save this driver. */
	    if (ndrivers >= adrivers) {
		  adrivers += 4;
		  drivers = realloc(drivers, adrivers*sizeof(ivl_nexus_ptr_t));
		  assert(drivers);
	    }
	    drivers[ndrivers] = nptr;
	    ndrivers += 1;
      }

      if (island_input_flag < 0)
	    island_input_flag = 0;

	/* Save the nexus driver count in the nex_data. */
      assert(nex_data);
      nex_data->drivers_count = ndrivers;
      nex_data->flags |= nex_flags;

	/* If the nexus has no drivers, then send a constant HiZ or
	   0.0 into the net. */
      if (ndrivers == 0) {
	      /* For real nets put 0.0. */
	    if (signal_data_type_of_nexus(nex) == IVL_VT_REAL) {
		  nex_private = draw_Cr_to_string(0.0);
	    } else {
		  unsigned jdx, wid = width_of_nexus(nex);
		  char*tmp = malloc(wid + 5);
		  nex_private = tmp;
		  strcpy(tmp, "C4<");
		  tmp += strlen(tmp);
		  switch (res) {
		      case IVL_SIT_TRI:
		      case IVL_SIT_UWIRE:
			for (jdx = 0 ;  jdx < wid ;  jdx += 1)
			      *tmp++ = 'z';
			break;
		      case IVL_SIT_TRI0:
			for (jdx = 0 ;  jdx < wid ;  jdx += 1)
			      *tmp++ = '0';
			break;
		      case IVL_SIT_TRI1:
			for (jdx = 0 ;  jdx < wid ;  jdx += 1)
			      *tmp++ = '1';
			break;
		      default:
			assert(0);
		  }
		  *tmp++ = '>';
		  *tmp = 0;

		    /* Create an "open" driver to hold the HiZ. We
		       need to do this so that .nets have something to
		       hang onto. */
		  char buf[64];
		  snprintf(buf, sizeof buf, "o%p", nex);
		  fprintf(vvp_out, "%s .functor BUFZ %u, %s; HiZ drive\n",
			  buf, wid, nex_private);
		  nex_private = realloc(nex_private, strlen(buf)+1);
		  strcpy(nex_private, buf);
	    }

	    if (island) {
		  char*tmp2 = draw_island_port(island, island_input_flag, nex, nex_data, nex_private);
		  free(nex_private);
		  nex_private = tmp2;
	    }
	    assert(nex_data->net_input == 0);
	    nex_data->net_input = nex_private;
	    return;
      }

	/* A uwire is a tri with only one driver. */
      if (res == IVL_SIT_UWIRE) {
	    if (ndrivers > 1) {
		  unsigned uidx;
		  ivl_signal_t usig = 0;
		    /* Find the uwire signal. */
		  for (uidx = 0 ;  uidx < ivl_nexus_ptrs(nex) ;  uidx += 1) {
			ivl_nexus_ptr_t ptr = ivl_nexus_ptr(nex, uidx);
			usig = ivl_nexus_ptr_sig(ptr);
			if (usig != 0) break;
		  }
		  assert(usig);

		  fprintf(stderr, "%s:%u: vvp.tgt error: uwire \"%s\" must "
		                  "have a single driver, found (%u).\n",
		                  ivl_signal_file(usig),
		                  ivl_signal_lineno(usig),
		                  ivl_signal_basename(usig), ndrivers);
		  vvp_errors += 1;
	    }
	    res = IVL_SIT_TRI;
      }

	/* If the nexus has exactly one driver, then simply draw
	   it. Note that this will *not* work if the nexus is not a
	   TRI type nexus. */
      if (ndrivers == 1 && res == IVL_SIT_TRI) {
	    ivl_signal_t path_sig = find_modpath(nex);
	    if (path_sig) {
		  char*nex_str = draw_net_input_drive(nex, drivers[0]);
		  char modpath_label[64];
		  snprintf(modpath_label, sizeof modpath_label,
			   "V_%p/m", path_sig);
		  nex_private = strdup(modpath_label);
		  draw_modpath(path_sig, nex_str);

	    } else {
		  nex_private = draw_net_input_drive(nex, drivers[0]);
	    }
	    if (island) {
		  char*tmp = draw_island_port(island, island_input_flag, nex, nex_data, nex_private);
		  free(nex_private);
		  nex_private = tmp;
	    }
	    assert(nex_data->net_input == 0);
	    nex_data->net_input = nex_private;
	    return;
      }

      level = 0;
      while (ndrivers) {
	    unsigned int inst;
	    for (inst = 0; inst < ndrivers; inst += 4) {
		  char*drive[4];
		  if (level == 0) {
			for (idx = inst; idx < ndrivers && idx < inst+4; idx += 1) {
			      drive[idx-inst] = draw_net_input_drive(nex, drivers[idx]);
			}
		  }

		  if (ndrivers > 4)
			fprintf(vvp_out, "RS_%p/%d/%d .resolv tri",
				nex, level, inst);
		  else
			fprintf(vvp_out, "RS_%p .resolv %s",
				nex, resolv_type);

		  for (idx = inst; idx < ndrivers && idx < inst+4; idx += 1) {
			if (level) {
			      fprintf(vvp_out, ", RS_%p/%d/%d",
				      nex, level - 1, idx*4);
			} else {
			      fprintf(vvp_out, ", %s", drive[idx-inst]);
			      free(drive[idx-inst]);
			}
		  }
		  for ( ;  idx < inst+4 ;  idx += 1) {
			fprintf(vvp_out, ", ");
			draw_C4_repeated_constant('z',width_of_nexus(nex));
		  }

		  fprintf(vvp_out, ";\n");
	    }
	    if (ndrivers > 4)
		  ndrivers = (ndrivers+3) / 4;
	    else
		  ndrivers = 0;
	    level += 1;
      }

      snprintf(result, sizeof result, "RS_%p", nex);

      if (island)
	    nex_private = draw_island_port(island, island_input_flag, nex, nex_data, result);
      else
	    nex_private = strdup(result);

      assert(nex_data->net_input == 0);
      nex_data->net_input = nex_private;
}

/*
 * Get a cached description of the nexus input, or create one if this
 * nexus has not been cached yet. This is a wrapper for the common
 * case call to draw_net_input_x.
 */
const char*draw_net_input(ivl_nexus_t nex)
{
      struct vvp_nexus_data*nex_data = (struct vvp_nexus_data*)
	    ivl_nexus_get_private(nex);

	/* If this nexus already has a label, then its input is
	   already figured out. Just return the existing label. */
      if (nex_data && nex_data->net_input)
	    return nex_data->net_input;

      if (nex_data == 0) {
	    nex_data = new_nexus_data();
	    ivl_nexus_set_private(nex, nex_data);
      }

      assert(nex_data->net_input == 0);
      draw_net_input_x(nex, nex_data);

      return nex_data->net_input;
}

const char*draw_island_net_input(ivl_island_t island, ivl_nexus_t nex)
{
      struct vvp_nexus_data*nex_data = (struct vvp_nexus_data*)
	    ivl_nexus_get_private(nex);

	/* If this nexus already has a label, then its input is
	   already figured out. Just return the existing label. */
      if (nex_data && nex_data->island_input) {
	    assert(nex_data->island == island);
	    return nex_data->island_input;
      }

      if (nex_data == 0) {
	    nex_data = new_nexus_data();
	    ivl_nexus_set_private(nex, nex_data);
      }

      assert(nex_data->net_input == 0);
      draw_net_input_x(nex, nex_data);

      assert(nex_data->island == island);
      assert(nex_data->island_input);

      return nex_data->island_input;
}
