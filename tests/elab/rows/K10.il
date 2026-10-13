# generated: elab-check --write
attribute \top 1
module \k10_task_no_args
  wire input 1 \clk
  wire width 4 output 2 \q
  wire $n6
  wire $n7
  wire $n8
  wire $n9
  process $proc$K10_task_without_arguments.v:9$4
    assign { $n9 $n8 $n7 $n6 } 4'0000
    sync posedge \clk
      update \q { $n9 $n8 $n7 $n6 }
  end
end
