/******************************************************************************

  Curse of War -- Real Time Strategy Game for Linux.
  Copyright (C) 2013 Alexey Nikolaev.

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 
******************************************************************************/
#include <unistd.h>
#include <stdio.h>
#include <curses.h>
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <aio.h>
#include <getopt.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include "common.h"
#include "grid.h"
#include "state.h"
#include "output.h"

/*****************************************************************************/
/*                           Global Constants                                */
/*****************************************************************************/

#define ESCAPE     '\033'
#define K_UP       65
#define K_DOWN     66
#define K_RIGHT    67
#define K_LEFT     68

volatile   sig_atomic_t   input_ready;
volatile   sig_atomic_t   time_to_redraw;
 
struct aiocb kbcbuf;        /* an aio control buf    */

void on_timer(int signum);   /* handler for alarm     */
void on_input(int);          /* handler for keybd     */
int  update_from_input( struct state *st );

/* SIGIO signal handler -- it is responsible for retrieving user input  */
void    setup_aio_buffer(struct aiocb *aio_buf);


void print_help() {
  printf(
"                                 __                      \n"
"    ____                        /  ]                     \n"
"   / __ \\_ _ ___ ___ ___    __ _| |_  /\\      /\\___ ___  \n"
" _/ /  \\/ | |X _/ __/ __\\  /   \\   /  \\ \\ /\\ / /__ \\X _/ \n"
" \\ X    | | | | |__ | __X  | X || |    \\ V  V // _ | |   \n"
"  \\ \\__/\\ __X_| \\___/___/  \\___/| |     \\ /\\ / \\___X_|   \n"
"   \\____/                       |/       V  V            \n"
"\n"
"  Written by Alexey Nikolaev in 2013.\n"
"\n");
  
  printf("  Command line arguments:\n\n");
  printf(
    "-W width\n"
    "\tMap width (default is 21)\n\n"
    "-H height\n"
    "\tMap height (default is 21)\n\n"
    "-S [rhombus|rect|hex]\n"
    "\tMap shape. It also sets N=4 for rhombus and rectangle, and N=6 for the hexagon.\n\n"
    "-l [2|3| ... N]\n"
    "\tSets L, the number of countries (default is N).\n\n"
    "-i [0|1|2|3|4]\n"
    "\tInequality between the countries (0 is the lowest, 4 in the highest).\n\n"
    "-q [1|2| ... L]\n"
    "\tChoose player's location by its quality (1 = the best available on the map, L = the worst).\n\n"
    "-r\n"
    "\tAbsolutely random initial conditions, overrides options -l, -i, and -q.\n\n"
    "-d [ee|e|n|h|hh]\n"
    "\tDifficulty level (AI) from the easiest to the hardest (default is normal).\n\n"
    "-s [p|sss|ss|s|n|f|ff|fff]\n"
    "\tGame speed from the slowest to the fastest (default is normal).\n\n"
    "-R seed\n"
    "\tSpecify a random seed (unsigned integer) for map generation.\n\n"
    "-h\n"
    "\tDisplay this help \n\n"
  );
}

void win_or_lose(struct state *st, int k) {
  int i, j, p;
  int pop[MAX_PLAYER];
  for (p=0; p<MAX_PLAYER; ++p) pop[p] = 0;

  for(i=0; i<st->grid.width; ++i){
    for(j=0; j<st->grid.height; ++j){
      if(is_inhabitable(st->grid.tiles[i][j].cl)) {
        for(p=0; p<MAX_PLAYER; ++p){
          pop[p] += st->grid.tiles[i][j].units[p][citizen];
        }
      }
    }
  }

  int win = 1;
  int lose = 0;
  int best = 0;
  for(p=0; p<MAX_PLAYER; ++p){
    if(pop[best] < pop[p]) best = p;
    if(p!=st->controlled && pop[p]>0) win = 0;
  }
  if(pop[st->controlled] == 0) lose = 1;

  if (win) {
    attrset(A_BOLD | COLOR_PAIR(4));
    mvaddstr(2 + st->grid.height, 31, "You are victorious!");
  }
  else if (lose) {
    attrset(A_BOLD | COLOR_PAIR(2));
    mvaddstr(2 + st->grid.height, 31, "You are defeated!");
  }
}

/* Run the game */
void run (struct state *st) {
  int k = 0;
  int finished = 0;
  while( !finished ) {
    if (time_to_redraw) {
      k++;
      if (k>=1600) k=0;

      int slowdown = 20;
      switch (st->speed) {
        case sp_pause: slowdown = 1; break;
        case sp_slowest: slowdown = 160; break;
        case sp_slower: slowdown = 80; break;
        case sp_slow: slowdown = 40; break;
        case sp_normal: slowdown = 20; break;
        case sp_fast: slowdown = 10; break;
        case sp_faster: slowdown = 5; break;
        case sp_fastest: slowdown = 2; break;
      }

      if (k % slowdown == 0 && st->speed != sp_pause) { 
        kings_move(st);
        simulate(st);
      }
      output_grid(st, k);
      time_to_redraw = 0;

      if (k%100 == 0) win_or_lose(st, k);
    }
    if ( input_ready ) {
      input_ready = 0;
      finished = update_from_input(st);
    }
    else
      pause();
  }
}

/*****************************************************************************/
/*                                    Main                                   */
/*****************************************************************************/
int main(int argc, char* argv[]){
  /* Initialize pseudo random number generator */
  srand(time(NULL));

  /* Parse argv */
  int r_flag = 0; // random
  int dif_val = dif_normal; // diffiulty
  int sp_val = sp_normal; // speed
  int w_val = 21; // width
  int h_val = 21; // height
  int l_val = 0;  // the number of starting locations
  unsigned int seed_val = rand();
  int conditions_val = 0;
  int conditions_were_set = 0;

  int ineq_val = RANDOM_INEQUALITY;
  enum stencil shape_val = st_rhombus;

	opterr = 0;
  int c;
	while ((c = getopt (argc, argv, "hrW:H:i:l:q:d:s:R:S:")) != -1){
		switch(c){
			case 'r': r_flag = 1; break;
			//case 'f': f_val = optarg; break;
			case 'W': { char* endptr = NULL;
									w_val = MAX(14, strtol(optarg, &endptr, 10));
									if (*endptr != '\0') {
                    print_help();
										return 1;
									}
								};
								break;
			case 'H': { char* endptr = NULL;
									h_val = MAX(14, strtol(optarg, &endptr, 10));
									if (*endptr != '\0') {
                    print_help();
										return 1;
									}
								};
								break;
			case 'i': { char* endptr = NULL;
									ineq_val = strtol(optarg, &endptr, 10);
									if (*endptr != '\0' || ineq_val < 0 || ineq_val > 4) {
                    print_help();
										return 1;
									}
								};
								break;
			case 'l': { char* endptr = NULL;
									l_val = strtol(optarg, &endptr, 10);
									if (*endptr != '\0') {
                    print_help();
										return 1;
									}
								};
								break;
			case 'q': { char* endptr = NULL;
									conditions_val = strtol(optarg, &endptr, 10);
                  conditions_were_set = 1;
									if (*endptr != '\0') {
                    print_help();
										return 1;
									}
								};
								break;
			case 'R': { char* endptr = NULL;
									seed_val = abs(strtol(optarg, &endptr, 10));
									if (*endptr != '\0' || *optarg == '\0') {
                    print_help();
										return 1;
									}
								};
								break;
      case 'd': if (strcmp(optarg, "n") == 0) dif_val = dif_normal;
                else if (strcmp(optarg, "e") == 0) dif_val = dif_easy;
                else if (strcmp(optarg, "e1") == 0) dif_val = dif_easy;
                else if (strcmp(optarg, "ee") == 0) dif_val = dif_easiest;
                else if (strcmp(optarg, "e2") == 0) dif_val = dif_easiest;
                else if (strcmp(optarg, "h") == 0) dif_val = dif_hard;
                else if (strcmp(optarg, "h1") == 0) dif_val = dif_hard;
                else if (strcmp(optarg, "hh") == 0) dif_val = dif_hardest;
                else if (strcmp(optarg, "h2") == 0) dif_val = dif_hardest;
                else {
                  print_help();
                  return 1;
                }
                break;
      case 's': if (strcmp(optarg, "n") == 0) sp_val = sp_normal;
                else if (strcmp(optarg, "s") == 0 || strcmp(optarg, "s1") == 0) sp_val = sp_slow;
                else if (strcmp(optarg, "ss") == 0 || strcmp(optarg, "s2") == 0) sp_val = sp_slower;
                else if (strcmp(optarg, "sss") == 0 || strcmp(optarg, "s3") == 0) sp_val = sp_slowest;
                else if (strcmp(optarg, "f") == 0 || strcmp(optarg, "f1") == 0) sp_val = sp_fast;
                else if (strcmp(optarg, "ff") == 0 || strcmp(optarg, "f2") == 0) sp_val = sp_faster;
                else if (strcmp(optarg, "fff") == 0 || strcmp(optarg, "f3") == 0) sp_val = sp_fastest;
                else if (strcmp(optarg, "p") == 0) sp_val = sp_pause;
                else {
                  print_help();
                  return 1;
                }
                break;
      case 'S': if (strcmp(optarg, "rhombus") == 0) shape_val = st_rhombus;
                else if (strcmp(optarg, "rect") == 0) shape_val = st_rect;
                else if (strcmp(optarg, "hex") == 0) shape_val = st_hex;
                else {
                  print_help();
                  return 1;
                }
                break;
      case '?': case 'h':
          print_help();
					return 1;
			default: abort ();
		}
	}
  /* Adjust l_val and conditions_val */
  {
    int avlbl_loc_num = stencil_avlbl_loc_num (shape_val);
    if(l_val == 0) l_val = avlbl_loc_num;

    if (l_val < 2 || l_val > avlbl_loc_num) {
      print_help();
      return 1;
    }
    if (conditions_were_set && (conditions_val<1 || conditions_val>l_val)) {
      print_help();
      return 1;
    }

    /*
	  l_val = IN_SEGMENT(l_val, 2, avlbl_loc_num);
    conditions_val = IN_SEGMENT(conditions_val, 1, avlbl_loc_num);
    */

    if (shape_val == st_rect) {
      w_val = MIN(MAX_WIDTH-1, w_val+(h_val+1)/2);
    }
  }

  
  struct sigaction newhandler;            /* new settings         */
  sigset_t         blocked;               /* set of blocked sigs  */

  newhandler.sa_handler = on_input;       /* handler function     */
  newhandler.sa_flags = SA_RESTART;       /* options     */

  /* then build the list of blocked signals  */
  sigemptyset(&blocked);                  /* clear all bits       */
  newhandler.sa_mask = blocked;           /* store blockmask      */
  if ( sigaction(SIGIO, &newhandler, NULL) == -1 )
    perror("sigaction");

  newhandler.sa_handler = on_timer;      /* handler function     */
  if ( sigaction(SIGALRM, &newhandler, NULL) == -1 )
    perror("sigaction");


  /* prepare the terminal for the animation */
  setlocale(LC_ALL, "");
  initscr();     /* initialize the library and screen */
  cbreak();      /* put terminal into non-blocking input mode */
  noecho();      /* turn off echo */
  start_color();
  clear();       /* clear the screen */
  curs_set(0);   /* hide the cursor */

  use_default_colors();
  init_pair(0, COLOR_WHITE, COLOR_BLACK);
  init_pair(1, COLOR_WHITE, COLOR_BLACK);
  init_pair(2, COLOR_BLACK, COLOR_BLACK);
  init_pair(3, COLOR_RED, COLOR_BLACK);
  init_pair(4, COLOR_GREEN, COLOR_BLACK);
  init_pair(5, COLOR_BLUE, COLOR_BLACK);
  init_pair(6, COLOR_YELLOW, COLOR_BLACK);
  init_pair(7, COLOR_MAGENTA, COLOR_BLACK);
  init_pair(8, COLOR_CYAN, COLOR_BLACK);
 
  /*
  int d = 9;
  int fbg = COLOR_BLUE;
  init_pair(d+0, COLOR_WHITE, fbg);
  init_pair(d+1, COLOR_WHITE, fbg);
  init_pair(d+2, COLOR_BLACK, fbg);
  init_pair(d+3, COLOR_RED, fbg);
  init_pair(d+4, COLOR_GREEN, fbg);
  init_pair(d+5, COLOR_BLUE, fbg);
  init_pair(d+6, COLOR_YELLOW, fbg);
  init_pair(d+7, COLOR_MAGENTA, fbg);
  init_pair(d+8, COLOR_CYAN, fbg);
  */

  color_set(0, NULL);
  assume_default_colors(COLOR_WHITE, COLOR_BLACK);
  clear();

  /* Initialize the parameters of the program */

  input_ready = 0;
  time_to_redraw = 1;

  attrset(A_BOLD | COLOR_PAIR(2));
  mvaddstr(0,0,"Map is generated. Please wait.");
  refresh();

  struct state st;
  state_init(&st, w_val, h_val, shape_val, seed_val, r_flag, l_val, conditions_val, ineq_val, sp_val, dif_val);
  
  clear();

  /* initialize aio buffer for the first read and place call */
  setup_aio_buffer(&kbcbuf);                         
  aio_read(&kbcbuf);                            

  /* Start the real time interval timer with delay interval size */
  //set_timer( ITIMER_REAL, 10, 10 );
  { 
    struct itimerval it;
    it.it_value.tv_sec = 0;
    it.it_value.tv_usec = 10000;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 10000;
    setitimer(ITIMER_REAL, &it, NULL);
  }
  refresh();        
  
  /* Run the game */
  run(&st);

  /* Restore the teminal state */
  echo();
  curs_set(1);
  clear();
  endwin();

  printf ("Random seed was %i\n", st.map_seed);
  return 0;
}


/*****************************************************************************/
/*                             SIGIO Signal Handler                          */
/*****************************************************************************/

void on_input(int signo)
{
    input_ready = 1;
}

/*  Handler called when aio_read() has stuff to read */
/*  First check for any error codes, and if ok, then get the return code */

int update_from_input( struct state *st )
{
    int c;
    char *cp = (char *) kbcbuf.aio_buf;      /* cast to char *  */
    int finished=0;

    /* check for errors  */
    if ( aio_error(&kbcbuf) != 0 )
        perror("reading failed");
    else 
        /* get number of chars read  */
        if ( aio_return(&kbcbuf) == 1 ) {
            c = *cp;
            int cursi = st->cursor.i;
            int cursj = st->cursor.j;
            /*ndelay = 0; */
            switch (c) {
                case 'Q':
                case 'q': 
                    finished = 1;                     /* quit program */
                    break;
                case 'f':
                    st->prev_speed = st->speed;
                    st->speed = faster(st->speed);
                    break;
                case 's':
                    st->prev_speed = st->speed;
                    st->speed = slower(st->speed);
                    break;
                case 'p':
                    if (st->speed == sp_pause)
                      st->speed = st->prev_speed;
                    else {
                      st->prev_speed = st->speed;
                      st->speed = sp_pause;
                    }
                    break;
                case 'h': case K_LEFT:
                  cursi--;
                  break;
                case 'l': case K_RIGHT:
                  cursi++;
                  break;
                case 'k': case K_UP:
                  cursj--;
                  if (cursj % 2 == 1)
                    cursi++;
                  break;
                case 'j': case K_DOWN:
                  cursj++;
                  if (cursj % 2 == 0)
                    cursi--;
                  break;
                case ' ':
                  if (st->fg[st->controlled].flag[st->cursor.i][st->cursor.j] == 0)
                    add_flag (&st->grid, &st->fg[st->controlled], st->cursor.i, st->cursor.j, FLAG_POWER);
                  else
                    remove_flag (&st->grid, &st->fg[st->controlled], st->cursor.i, st->cursor.j, FLAG_POWER);
                  break;
                case 'x':
                  remove_flags_with_prob (&st->grid, &st->fg[st->controlled], 1.0);
                  break;
                case 'c':
                  remove_flags_with_prob (&st->grid, &st->fg[st->controlled], 0.5);
                  break;
                case 'r':
                case 'v':
                  build (&st->grid, &st->country[st->controlled], st->controlled, st->cursor.i, st->cursor.j);
                  break;
                
                case ESCAPE:
                case 91:
                  break;
                }
            
            cursi = IN_SEGMENT(cursi, 0, st->grid.width-1);
            cursj = IN_SEGMENT(cursj, 0, st->grid.height-1);
            if ( is_visible(st->grid.tiles[cursi][cursj].cl) ) {
              st->cursor.i = cursi;
              st->cursor.j = cursj;
            }

        }                
    /* place a new request  */
    aio_read(&kbcbuf);
    return finished;
}

/*****************************************************************************/
/*                           SIGALRM Signal Handler                          */
/*****************************************************************************/

/* SIGALRM handler -- moves string on the screen when the signal is received */
void on_timer(int signum)
{
    time_to_redraw = 1;
}

/*****************************************************************************/
/*                      Asynchronous I/O Library Setup                       */
/*****************************************************************************/


/*  The following function initializes the AIO structure to enable */
/*  asynchronous I/O through the AIO library. */
void setup_aio_buffer(struct aiocb *aio_buf)
{
    static char input[1];              /* 1 char of input  */

    /* describe what to read  */
    aio_buf->aio_fildes     = 0;       /* file descriptor for I/O */
    aio_buf->aio_buf        = input;   /* address of buffer for I/O           */
    aio_buf->aio_nbytes     = 1;       /* number of bytes to read  each time */
    aio_buf->aio_offset     = 0;       /* offset in file to start reads   */

    /* describe what to do when read is ready  */
    aio_buf->aio_sigevent.sigev_notify = SIGEV_SIGNAL; 
    aio_buf->aio_sigevent.sigev_signo  = SIGIO;       /* send SIGIO    */
}

