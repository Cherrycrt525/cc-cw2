/* Copyright (C) 2017 Daniel Page <csdsp@bristol.ac.uk>
 *
 * Use of this source code is restricted per the CC BY-NC-ND license, a copy of
 * which can be found via http://creativecommons.org (and should be included as
 * LICENSE.txt within the associated archive or repository).
 */

#include "hilevel.h"

pcb_t pcb[ MAX_PROCS ];
pcb_t* executing = NULL;

void dispatch( ctx_t* ctx, pcb_t* prev, pcb_t* next ) {
  char prev_pid = '?', next_pid = '?';

  if( NULL != prev ) {
    memcpy( &prev->ctx, ctx, sizeof( ctx_t ) );
    prev_pid = '0' + prev->pid;
  }
  if( NULL != next ) {
    memcpy( ctx, &next->ctx, sizeof( ctx_t ) );
    next_pid = '0' + next->pid;
  }

  // print context switch
    PL011_putc( UART0, '[',      true );
    PL011_putc( UART0, prev_pid, true );
    PL011_putc( UART0, '-',      true );
    PL011_putc( UART0, '>',      true );
    PL011_putc( UART0, next_pid, true );
    PL011_putc( UART0, ']',      true );

    PL011_putc( UART0, '\n',     true );

    executing = next;
    executing->age = 0;

  return;
}

void Round_Robin ( ctx_t* ctx, int next ){
  if( executing->status != STATUS_TERMINATED && pcb[ next ].status == STATUS_READY ) {
    executing->status = STATUS_READY;
    pcb[ next ].status = STATUS_EXECUTING;
  }
  if ( executing->status == STATUS_TERMINATED ) {
    pcb[ next ].status = STATUS_TERMINATED;
  }
  dispatch( ctx, executing, &pcb[ next ] );
}

void schedule( ctx_t* ctx ) {
  int next_exec;
  int max = -1;

  for ( int i=0; i<MAX_PROCS; i++ ) {
    if ( pcb[ i ].age + pcb[ i ].priority > max ) {
      max = pcb[ i ].age + pcb[ i ].priority;
      next_exec = i;
    }
  }

  for ( int i=0; i<MAX_PROCS; i++ ) {
    if ( i == next_exec || pcb[ i ].status == STATUS_CREATED || pcb[ i ].status == STATUS_TERMINATED ) {
      continue;
    }
    pcb[ i ].age += 1;
  }
  pcb[ next_exec ].age = 0;

  Round_Robin( ctx, next_exec );

}

pcb_t* get_next_empty(){
  pcb_t* next_empty = NULL;
  for ( int i=0; i<MAX_PROCS; i++ ) {
    if ( pcb[ i ].status == STATUS_CREATED ) {
      next_empty = &pcb[ i ];
    break;
    }
  }
  return next_empty;
}

extern void     main_P3();
extern uint32_t tos_P3;
extern void     main_P4();
extern uint32_t tos_P4;
extern void     main_P5();
extern uint32_t tos_P5;
extern void     main_console();
extern uint32_t tos_csl;
extern uint32_t tos_str;


void hilevel_handler_rst( ctx_t* ctx ) {
  for ( int i=0; i<MAX_PROCS; i++ ) {
    pcb[ i ].status = STATUS_INVALID;
  }

  memset( &pcb[ 0 ], 0, sizeof( pcb_t ) );
  pcb[ 0 ].pid      = 0;
  pcb[ 0 ].status   = STATUS_READY;
  pcb[ 0 ].tos      = ( uint32_t )( &tos_csl );
  pcb[ 0 ].ctx.cpsr = 0x50;
  pcb[ 0 ].ctx.pc   = ( uint32_t )( &main_console );
  pcb[ 0 ].ctx.sp   = pcb[ 0 ].tos;
  pcb[ 0 ].priority = 2;
  pcb[ 0 ].age      = 0;

  for ( int i=1; i<MAX_PROCS; i++) {
    memset( &pcb[ i ], 0, sizeof( pcb_t ) );
    pcb[ i ].pid      = i;
    pcb[ i ].status   = STATUS_CREATED;
    pcb[ i ].tos      = ( uint32_t )( ( &tos_csl) - ( i * 0x00001000 ) );
    pcb[ i ].ctx.cpsr = 0x50;
    pcb[ i ].ctx.sp   = pcb[ i ].tos;
    pcb[ i ].priority = 1;
    pcb[ i ].age      = 0;
  }

  TIMER0->Timer1Load  = 0x00100000; // select period = 2^20 ticks ~= 1 sec
  TIMER0->Timer1Ctrl  = 0x00000002; // select 32-bit   timer
  TIMER0->Timer1Ctrl |= 0x00000040; // select periodic timer
  TIMER0->Timer1Ctrl |= 0x00000020; // enable          timer interrupt
  TIMER0->Timer1Ctrl |= 0x00000080; // enable          timer

  GICC0->PMR          = 0x000000F0; // unmask all            interrupts
  GICD0->ISENABLER1  |= 0x00000010; // enable timer          interrupt
  GICC0->CTLR         = 0x00000001; // enable GIC interface
  GICD0->CTLR         = 0x00000001; // enable GIC distributor

  dispatch( ctx, NULL, &pcb[ 0 ] ); //& presenting the add.

  int_enable_irq();

  return;
}

void hilevel_handler_irq(ctx_t* ctx) {
  // step 2: read the interrupt identifier to know the source

  uint32_t id = GICC0->IAR;

  // step 4: handle the interrupt, then clear/reset the source

  if ( id == GIC_SOURCE_TIMER0 ) {
    //reset the timer
    TIMER0->Timer1IntClr = 0x01;

    schedule( ctx );
  }

  // setp 5: write the interrupt identifier to signal

  GICC0->EOIR = id;

  return;
}

void hilevel_handler_svc(ctx_t* ctx, uint32_t id ) {

  switch ( id ) {

    case 0x00/* value */: { // 0x00: yield()
      schedule( ctx );

      break;
    }

    case 0x01 : { // 0x01: write( fd, x, n )
      int   fd = ( int   )( ctx->gpr[ 0 ] );
      char*  x = ( char* )( ctx->gpr[ 1 ] );
      int    n = ( int   )( ctx->gpr[ 2 ] );

      for ( int i=0; i<n; i++ ) {
        PL011_putc( UART0, *x++, true );
      }

      ctx->gpr[ 0 ] = n;

      break;
    }

    case 0x03 : { // 0x03: fork()
      PL011_putc( UART0, '[',      true );
      PL011_putc( UART0, 'F',      true );
      PL011_putc( UART0, 'O',      true );
      PL011_putc( UART0, 'R',      true );
      PL011_putc( UART0, 'K',      true );
      PL011_putc( UART0, ']',      true );

      pcb[ 0 ].priority += 1;

      pcb_t* child = get_next_empty();
      if ( child != NULL ) {
        memcpy( &child->ctx, ctx, sizeof( ctx_t ) );
        child->status = STATUS_READY;

        uint32_t sp_offset = ( uint32_t ) &executing->tos - ctx->sp;
        child->ctx.sp = child->tos - sp_offset;
        memcpy( ( void* ) child->ctx.sp, ( void* ) ctx->sp, sp_offset );

        child->ctx.gpr[ 0 ] = 0;
        ctx->gpr[ 0 ] = child->pid;

        break;
      }
      else {
        ctx->gpr[ 0 ] = executing->pid;
        break;
      }

    }

    case 0x04 : {// 0x04: exit()
      PL011_putc( UART0, '[', true );
      PL011_putc( UART0, 'E', true );
      PL011_putc( UART0, 'X', true );
      PL011_putc( UART0, 'I', true );
      PL011_putc( UART0, 'T', true );
      PL011_putc( UART0, ']', true );

      executing->status = STATUS_TERMINATED;
      executing->age    = 0;
      schedule( ctx );
      pcb[ 0 ].priority -= 1;

      break;
    }

    case 0x05 : { //0x05: exec()
      PL011_putc( UART0, '[', true );
      PL011_putc( UART0, 'E', true );
      PL011_putc( UART0, 'X', true );
      PL011_putc( UART0, 'E', true );
      PL011_putc( UART0, 'C', true );
      PL011_putc( UART0, ']', true );
      ctx->pc = ctx->gpr[0];

      break;
    }

    default : {// 0x??: unknown / unsupported

      break;
    }
  }

  return;
}
