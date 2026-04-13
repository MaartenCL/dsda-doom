//
// Copyright(C) 2026 by The DSDA-Doom Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//   DSDA Viewcam Script
//

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "i_system.h"
#include "lprintf.h"
#include "m_file.h"
#include "m_misc.h"
#include "z_zone.h"

#include "dsda/utility.h"
#include "dsda/viewcam.h"

typedef enum {
  dsda_viewcam_action_static,
  dsda_viewcam_action_linear,
  dsda_viewcam_action_arc,
  dsda_viewcam_action_bezier,
} dsda_viewcam_action_t;

typedef enum {
  dsda_viewcam_orientation_absolute,
  dsda_viewcam_orientation_movement,
  dsda_viewcam_orientation_center,
} dsda_viewcam_orientation_t;

typedef struct {
  int first_tic;
  int last_tic;
  dsda_viewcam_action_t action;
  dsda_viewcam_orientation_t orientation;

  union {
    struct {
      fixed_t x;
      fixed_t y;
      fixed_t z;
      angle_t angle;
    } static_action;

    struct {
      fixed_t x1;
      fixed_t y1;
      fixed_t z1;
      angle_t angle_start;
      fixed_t x2;
      fixed_t y2;
      fixed_t z2;
      angle_t angle_delta;
    } linear;

    struct {
      fixed_t cx;
      fixed_t cy;
      fixed_t radius;
      fixed_t z1;
      fixed_t z2;
      float rot_start;
      float rot_delta;
      angle_t angle_start;
      angle_t angle_delta;
    } arc;

    struct {
      int point_count;
      fixed_t *points;
      angle_t angle_start;
      angle_t angle_delta;
    } bezier;
  } data;
} dsda_viewcam_instruction_t;

static dsda_viewcam_instruction_t *dsda_viewcam = NULL;
static int dsda_viewcam_count = 0;

#define DSDA_MAX_VIEWCAM_TOKENS 512
#define DSDA_PI 3.14159265358979323846f

static void dsda_ViewcamScriptError(const char *path, int line_number, const char *format, ...)
{
  char reason[512];
  va_list args;

  va_start(args, format);
  vsnprintf(reason, sizeof(reason), format, args);
  va_end(args);

  I_Error("Invalid viewcam script %s:%d: %s", path, line_number, reason);
}

static dboolean dsda_ParseIntToken(const char *token, int *out)
{
  char *end;
  long value;

  errno = 0;
  value = strtol(token, &end, 10);

  if (errno || token == end || *end != '\0')
    return false;

  if (value < INT_MIN || value > INT_MAX)
    return false;

  *out = (int) value;

  return true;
}

static dboolean dsda_ParseFloatToken(const char *token, float *out)
{
  char *end;

  errno = 0;
  *out = strtof(token, &end);

  if (errno || token == end || *end != '\0')
    return false;

  return true;
}

static dboolean dsda_ParseFixedToken(const char *token, fixed_t *out)
{
  float value;

  if (!dsda_ParseFloatToken(token, &value))
    return false;

  *out = dsda_FloatToFixed(value);

  return true;
}

static dboolean dsda_ParseAngleToken(const char *token, angle_t *out)
{
  float value;

  if (!dsda_ParseFloatToken(token, &value))
    return false;

  *out = dsda_DegreesToAngle(value);

  return true;
}

static dsda_viewcam_orientation_t dsda_ParseOrientationToken(const char *token, dboolean allow_center)
{
  if (!strcasecmp(token, "absolute"))
    return dsda_viewcam_orientation_absolute;

  if (!strcasecmp(token, "movement"))
    return dsda_viewcam_orientation_movement;

  if (allow_center && !strcasecmp(token, "center"))
    return dsda_viewcam_orientation_center;

  return -1;
}

static char *dsda_LTrim(char *line)
{
  while (*line && isspace((unsigned char) *line))
    ++line;

  return line;
}

static int dsda_SplitLine(char *line, char **tokens, int max_tokens)
{
  int count = 0;
  char *token;
  char *save_ptr = NULL;

  token = strtok_r(line, " \t\r", &save_ptr);

  while (token != NULL && count < max_tokens)
  {
    if (token[0] == '#')
      break;

    tokens[count++] = token;
    token = strtok_r(NULL, " \t\r", &save_ptr);
  }

  return count;
}

static fixed_t dsda_InterpolateFixed(fixed_t a, fixed_t b, double t)
{
  return a + (fixed_t) M_DoubleToInt((double) (b - a) * t);
}

static angle_t dsda_ApplyAngleDelta(angle_t start, angle_t delta, double t)
{
  return (angle_t) (start + (int32_t) M_DoubleToInt((int32_t) delta * t));
}

static angle_t dsda_DirectionAngle(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
  float dx;
  float dy;
  float degrees;

  dx = (float) (x2 - x1) / FRACUNIT;
  dy = (float) (y2 - y1) / FRACUNIT;

  if (dx == 0.f && dy == 0.f)
    return 0;

  degrees = (float) ((atan2f(dy, dx) * 180.0f) / DSDA_PI);

  return dsda_DegreesToAngle(degrees);
}

static angle_t dsda_DirectionAngleFromVector(double dx, double dy)
{
  float degrees;

  if (dx == 0.0 && dy == 0.0)
    return 0;

  degrees = (float) ((atan2(dy, dx) * 180.0) / DSDA_PI);

  return dsda_DegreesToAngle(degrees);
}

static void dsda_EvaluateBezierPoint(const fixed_t *points, int point_count, double t,
                                     fixed_t *x, fixed_t *y, fixed_t *z)
{
  int degree;
  double inv_t;
  double basis;
  double ratio;
  double px, py, pz;
  int i;

  if (point_count <= 0)
  {
    *x = *y = *z = 0;
    return;
  }

  if (t <= 0.0)
  {
    *x = points[0];
    *y = points[1];
    *z = points[2];
    return;
  }

  if (t >= 1.0)
  {
    int base = (point_count - 1) * 3;
    *x = points[base + 0];
    *y = points[base + 1];
    *z = points[base + 2];
    return;
  }

  degree = point_count - 1;
  inv_t = 1.0 - t;
  basis = pow(inv_t, degree);
  ratio = t / inv_t;

  px = py = pz = 0.0;

  for (i = 0; i <= degree; ++i)
  {
    int base = i * 3;

    px += basis * ((double) points[base + 0] / FRACUNIT);
    py += basis * ((double) points[base + 1] / FRACUNIT);
    pz += basis * ((double) points[base + 2] / FRACUNIT);

    if (i < degree)
      basis *= ((double) (degree - i) / (double) (i + 1)) * ratio;
  }

  *x = dsda_FloatToFixed((float) px);
  *y = dsda_FloatToFixed((float) py);
  *z = dsda_FloatToFixed((float) pz);
}

static void dsda_EvaluateBezierDerivative(const fixed_t *points, int point_count, double t,
                                          double *dx, double *dy)
{
  int degree;
  int ddegree;
  double inv_t;
  double basis;
  double ratio;
  double vx, vy;
  int i;

  if (point_count <= 1)
  {
    *dx = 0.0;
    *dy = 0.0;
    return;
  }

  degree = point_count - 1;

  if (t <= 0.0)
  {
    *dx = degree * ((double) (points[3] - points[0]) / FRACUNIT);
    *dy = degree * ((double) (points[4] - points[1]) / FRACUNIT);
    return;
  }

  if (t >= 1.0)
  {
    int p0 = (point_count - 2) * 3;
    int p1 = (point_count - 1) * 3;
    *dx = degree * ((double) (points[p1 + 0] - points[p0 + 0]) / FRACUNIT);
    *dy = degree * ((double) (points[p1 + 1] - points[p0 + 1]) / FRACUNIT);
    return;
  }

  ddegree = degree - 1;
  inv_t = 1.0 - t;
  basis = pow(inv_t, ddegree);
  ratio = t / inv_t;

  vx = vy = 0.0;

  for (i = 0; i <= ddegree; ++i)
  {
    int p0 = i * 3;
    int p1 = (i + 1) * 3;

    vx += basis * ((double) (points[p1 + 0] - points[p0 + 0]) / FRACUNIT);
    vy += basis * ((double) (points[p1 + 1] - points[p0 + 1]) / FRACUNIT);

    if (i < ddegree)
      basis *= ((double) (ddegree - i) / (double) (i + 1)) * ratio;
  }

  *dx = degree * vx;
  *dy = degree * vy;
}

void dsda_ClearViewcamScript(void)
{
  int i;

  for (i = 0; i < dsda_viewcam_count; ++i)
  {
    if (dsda_viewcam[i].action == dsda_viewcam_action_bezier)
      Z_Free(dsda_viewcam[i].data.bezier.points);
  }

  Z_Free(dsda_viewcam);
  dsda_viewcam = NULL;
  dsda_viewcam_count = 0;
}

void dsda_LoadViewcamScript(const char *path)
{
  char *buffer = NULL;
  char *line;
  char *next_line;
  int line_number;

  dsda_ClearViewcamScript();

  if (M_ReadFileToString(path, &buffer) == -1)
    I_Error("Could not read viewcam script file %s", path);

  line = buffer;
  line_number = 1;

  while (line != NULL && *line)
  {
    char *line_start;
    char *tokens[DSDA_MAX_VIEWCAM_TOKENS];
    int token_count;
    dsda_viewcam_instruction_t instruction;

    next_line = strchr(line, '\n');

    if (next_line)
    {
      *next_line = '\0';
      ++next_line;
    }

    line_start = dsda_LTrim(line);
    M_StrRTrim(line_start);

    if (*line_start == '\0' || *line_start == '#')
    {
      line = next_line;
      ++line_number;
      continue;
    }

    token_count = dsda_SplitLine(line_start, tokens, DSDA_MAX_VIEWCAM_TOKENS);

    if (token_count < 3)
      dsda_ViewcamScriptError(path, line_number, "expected at least 3 tokens");

    memset(&instruction, 0, sizeof(instruction));

    if (!dsda_ParseIntToken(tokens[0], &instruction.first_tic))
      dsda_ViewcamScriptError(path, line_number, "invalid first tic '%s'", tokens[0]);

    if (!dsda_ParseIntToken(tokens[1], &instruction.last_tic))
      dsda_ViewcamScriptError(path, line_number, "invalid last tic '%s'", tokens[1]);

    if (instruction.first_tic > instruction.last_tic)
      dsda_ViewcamScriptError(path, line_number, "first tic must be <= last tic");

    if (!strcasecmp(tokens[2], "static"))
    {
      instruction.action = dsda_viewcam_action_static;
      instruction.orientation = dsda_viewcam_orientation_absolute;

      if (token_count != 7)
        dsda_ViewcamScriptError(path, line_number, "static expects 7 tokens");

      if (!dsda_ParseFixedToken(tokens[3], &instruction.data.static_action.x) ||
          !dsda_ParseFixedToken(tokens[4], &instruction.data.static_action.y) ||
          !dsda_ParseFixedToken(tokens[5], &instruction.data.static_action.z) ||
          !dsda_ParseAngleToken(tokens[6], &instruction.data.static_action.angle))
        dsda_ViewcamScriptError(path, line_number, "invalid static parameters");
    }
    else if (!strcasecmp(tokens[2], "linear"))
    {
      instruction.action = dsda_viewcam_action_linear;
      instruction.orientation = dsda_viewcam_orientation_absolute;

      if (token_count != 11 && token_count != 12)
        dsda_ViewcamScriptError(path, line_number, "linear expects 11 or 12 tokens");

      if (!dsda_ParseFixedToken(tokens[3], &instruction.data.linear.x1) ||
          !dsda_ParseFixedToken(tokens[4], &instruction.data.linear.y1) ||
          !dsda_ParseFixedToken(tokens[5], &instruction.data.linear.z1) ||
          !dsda_ParseFixedToken(tokens[6], &instruction.data.linear.x2) ||
          !dsda_ParseFixedToken(tokens[7], &instruction.data.linear.y2) ||
          !dsda_ParseFixedToken(tokens[8], &instruction.data.linear.z2) ||
          !dsda_ParseAngleToken(tokens[9], &instruction.data.linear.angle_start) ||
          !dsda_ParseAngleToken(tokens[10], &instruction.data.linear.angle_delta))
        dsda_ViewcamScriptError(path, line_number, "invalid linear parameters");

      if (token_count == 12)
      {
        instruction.orientation = dsda_ParseOrientationToken(tokens[11], false);

        if (instruction.orientation == -1)
          dsda_ViewcamScriptError(path, line_number, "invalid linear orientation '%s'", tokens[11]);
      }
    }
    else if (!strcasecmp(tokens[2], "arc"))
    {
      instruction.action = dsda_viewcam_action_arc;
      instruction.orientation = dsda_viewcam_orientation_absolute;

      if (token_count != 12 && token_count != 13)
        dsda_ViewcamScriptError(path, line_number, "arc expects 12 or 13 tokens");

      if (!dsda_ParseFixedToken(tokens[3], &instruction.data.arc.cx) ||
          !dsda_ParseFixedToken(tokens[4], &instruction.data.arc.cy) ||
          !dsda_ParseFixedToken(tokens[5], &instruction.data.arc.radius) ||
          !dsda_ParseFloatToken(tokens[6], &instruction.data.arc.rot_start) ||
          !dsda_ParseFloatToken(tokens[7], &instruction.data.arc.rot_delta) ||
          !dsda_ParseFixedToken(tokens[8], &instruction.data.arc.z1) ||
          !dsda_ParseFixedToken(tokens[9], &instruction.data.arc.z2) ||
          !dsda_ParseAngleToken(tokens[10], &instruction.data.arc.angle_start) ||
          !dsda_ParseAngleToken(tokens[11], &instruction.data.arc.angle_delta))
        dsda_ViewcamScriptError(path, line_number, "invalid arc parameters");

      if (token_count == 13)
      {
        instruction.orientation = dsda_ParseOrientationToken(tokens[12], true);

        if (instruction.orientation == -1)
          dsda_ViewcamScriptError(path, line_number, "invalid arc orientation '%s'", tokens[12]);
      }
    }
    else if (!strcasecmp(tokens[2], "bezier"))
    {
      int point_count;
      int token_count_without_orientation;
      int token_count_with_orientation;
      int angle_start_index;
      int angle_delta_index;
      int i;

      instruction.action = dsda_viewcam_action_bezier;
      instruction.orientation = dsda_viewcam_orientation_absolute;

      if (token_count < 12)
        dsda_ViewcamScriptError(path, line_number, "bezier expects at least 12 tokens");

      if (!dsda_ParseIntToken(tokens[3], &point_count))
        dsda_ViewcamScriptError(path, line_number, "invalid bezier point count '%s'", tokens[3]);

      if (point_count < 2)
        dsda_ViewcamScriptError(path, line_number, "bezier point count must be at least 2");

      token_count_without_orientation = 6 + point_count * 3;
      token_count_with_orientation = token_count_without_orientation + 1;

      if (token_count != token_count_without_orientation && token_count != token_count_with_orientation)
        dsda_ViewcamScriptError(path, line_number, "bezier token count does not match point count");

      instruction.data.bezier.point_count = point_count;
      instruction.data.bezier.points = Z_Malloc(point_count * 3 * sizeof(fixed_t));

      for (i = 0; i < point_count * 3; ++i)
      {
        if (!dsda_ParseFixedToken(tokens[4 + i], &instruction.data.bezier.points[i]))
          dsda_ViewcamScriptError(path, line_number, "invalid bezier control point parameter");
      }

      angle_start_index = 4 + point_count * 3;
      angle_delta_index = angle_start_index + 1;

      if (!dsda_ParseAngleToken(tokens[angle_start_index], &instruction.data.bezier.angle_start) ||
          !dsda_ParseAngleToken(tokens[angle_delta_index], &instruction.data.bezier.angle_delta))
        dsda_ViewcamScriptError(path, line_number, "invalid bezier angle parameters");

      if (token_count == token_count_with_orientation)
      {
        instruction.orientation = dsda_ParseOrientationToken(tokens[angle_delta_index + 1], false);

        if (instruction.orientation == -1)
          dsda_ViewcamScriptError(path, line_number, "invalid bezier orientation '%s'", tokens[angle_delta_index + 1]);
      }
    }
    else
      dsda_ViewcamScriptError(path, line_number, "unknown action '%s'", tokens[2]);

    ++dsda_viewcam_count;
    dsda_viewcam = Z_Realloc(
      dsda_viewcam,
      dsda_viewcam_count * sizeof(*dsda_viewcam)
    );
    dsda_viewcam[dsda_viewcam_count - 1] = instruction;

    line = next_line;
    ++line_number;
  }

  Z_Free(buffer);

  if (!dsda_viewcam_count)
    I_Error("Viewcam script file %s has no instructions", path);
}

dboolean dsda_HasViewcamScript(void)
{
  return dsda_viewcam_count > 0;
}

dboolean dsda_EvaluateViewcamScript(int tic, fixed_t *x, fixed_t *y, fixed_t *z, angle_t *angle)
{
  const dsda_viewcam_instruction_t *instruction = NULL;
  int i;
  double t;
  angle_t offset;

  for (i = 0; i < dsda_viewcam_count; ++i)
  {
    if (tic >= dsda_viewcam[i].first_tic && tic <= dsda_viewcam[i].last_tic)
      instruction = &dsda_viewcam[i];
  }

  if (!instruction)
    return false;

  if (instruction->last_tic == instruction->first_tic)
    t = 0.0;
  else
    t = (double) (tic - instruction->first_tic) /
        (double) (instruction->last_tic - instruction->first_tic);

  if (t < 0.0)
    t = 0.0;
  if (t > 1.0)
    t = 1.0;

  switch (instruction->action)
  {
    case dsda_viewcam_action_static:
      *x = instruction->data.static_action.x;
      *y = instruction->data.static_action.y;
      *z = instruction->data.static_action.z;
      *angle = instruction->data.static_action.angle;
      return true;

    case dsda_viewcam_action_linear:
      *x = dsda_InterpolateFixed(instruction->data.linear.x1, instruction->data.linear.x2, t);
      *y = dsda_InterpolateFixed(instruction->data.linear.y1, instruction->data.linear.y2, t);
      *z = dsda_InterpolateFixed(instruction->data.linear.z1, instruction->data.linear.z2, t);

      offset = dsda_ApplyAngleDelta(
        instruction->data.linear.angle_start,
        instruction->data.linear.angle_delta,
        t
      );

      if (instruction->orientation == dsda_viewcam_orientation_movement)
        *angle = dsda_DirectionAngle(
          instruction->data.linear.x1,
          instruction->data.linear.y1,
          instruction->data.linear.x2,
          instruction->data.linear.y2
        ) + offset;
      else
        *angle = offset;

      return true;

    case dsda_viewcam_action_arc:
    {
      float theta_deg;
      float theta_rad;
      float cx;
      float cy;
      float radius;
      angle_t base_angle;

      theta_deg = instruction->data.arc.rot_start + instruction->data.arc.rot_delta * (float) t;
      theta_rad = theta_deg * DSDA_PI / 180.0f;

      cx = (float) instruction->data.arc.cx / FRACUNIT;
      cy = (float) instruction->data.arc.cy / FRACUNIT;
      radius = (float) instruction->data.arc.radius / FRACUNIT;

      *x = dsda_FloatToFixed(cx + radius * cosf(theta_rad));
      *y = dsda_FloatToFixed(cy + radius * sinf(theta_rad));
      *z = dsda_InterpolateFixed(instruction->data.arc.z1, instruction->data.arc.z2, t);

      offset = dsda_ApplyAngleDelta(
        instruction->data.arc.angle_start,
        instruction->data.arc.angle_delta,
        t
      );

      if (instruction->orientation == dsda_viewcam_orientation_movement)
      {
        if (instruction->data.arc.rot_delta < 0)
          base_angle = dsda_DegreesToAngle(theta_deg - 90.0f);
        else
          base_angle = dsda_DegreesToAngle(theta_deg + 90.0f);
      }
      else if (instruction->orientation == dsda_viewcam_orientation_center)
        base_angle = dsda_DirectionAngle(*x, *y, instruction->data.arc.cx, instruction->data.arc.cy);
      else
        base_angle = 0;

      *angle = base_angle + offset;
      return true;
    }

    case dsda_viewcam_action_bezier:
    {
      double dx;
      double dy;
      angle_t base_angle;

      dsda_EvaluateBezierPoint(
        instruction->data.bezier.points,
        instruction->data.bezier.point_count,
        t,
        x,
        y,
        z
      );

      offset = dsda_ApplyAngleDelta(
        instruction->data.bezier.angle_start,
        instruction->data.bezier.angle_delta,
        t
      );

      if (instruction->orientation == dsda_viewcam_orientation_movement)
      {
        dsda_EvaluateBezierDerivative(
          instruction->data.bezier.points,
          instruction->data.bezier.point_count,
          t,
          &dx,
          &dy
        );
        base_angle = dsda_DirectionAngleFromVector(dx, dy);
      }
      else
        base_angle = 0;

      *angle = base_angle + offset;
      return true;
    }
  }

  return false;
}
