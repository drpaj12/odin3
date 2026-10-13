# generated: elab-check --write
attribute \top 1
module \t17_named_blocks
  wire input 1 \clk
  wire input 2 \d
  wire output 3 \q
  wire output 4 \r
  wire $n5
  wire $n6
  process $proc$T17_named_blocks.v:3$5
    assign $n5 \d
    sync posedge \clk
      update \q $n5
  end
  process $proc$T17_named_blocks.v:6$6
    assign $n6 \q
    sync posedge \clk
      update \r $n6
  end
end
