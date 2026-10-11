/* Two operators resolved by precedence: four solved conflicts (test_solved_conflicts.py). */
%token N
%left '+'
%left '*'
%%
e: e '+' e | e '*' e | N ;
