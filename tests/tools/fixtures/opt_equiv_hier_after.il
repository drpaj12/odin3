attribute \top 1
module \bm_if_collapse
  wire input 1 \clock
  wire input 2 \reset_n
  wire width 2 input 3 \a_in
  wire width 2 input 4 \b_in
  wire input 5 \c_in
  wire input 6 \d_in
  wire width 2 output 7 \out0
  wire width 2 output 8 \out2
  wire output 9 \out1
  wire width 2 \temp_a
  wire $n16
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n22
  wire $n25
  wire $n26
  wire $n27
  cell \a \top_a
    connect \clock \clock
    connect \a_in \a_in
    connect \b_in \b_in
    connect \out \temp_a
  end
  cell $and $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 2
    connect \A \a_in
    connect \B \b_in
    connect \Y { $n26 $n25 }
  end
  cell $and $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \c_in
    connect \B \d_in
    connect \Y $n27
  end
  cell $not $c18
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \c_in
    connect \Y $n22
  end
  process $proc$/home/jamiespa/odin3-ws/odin3-2F3/tests/micro/verilog/micro/bm_if_collapse.v:35$17
    assign { $n17 $n16 } \out0
    assign $n20 \out1
    assign { $n19 $n18 } \temp_a
    switch $n22
      case 1'1
        assign { $n17 $n16 } 2'00
        assign $n20 1'0
      case
        switch \d_in
          case 1'1
            assign { $n17 $n16 } { $n26 $n25 }
            assign $n20 $n27
        end
    end
    sync posedge \clock
      update \out0 { $n17 $n16 }
      update \out2 { $n19 $n18 }
      update \out1 $n20
  end
end
module \a
  wire input 1 \clock
  wire width 2 input 2 \a_in
  wire width 2 input 3 \b_in
  wire width 2 output 4 \out
  wire width 2 \out1
  wire width 2 \out2
  wire $n12
  wire $n13
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n20
  wire $n21
  wire $n22
  cell $ne $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A \b_in
    connect \B 2'01
    connect \Y $n20
  end
  cell $and $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 2
    parameter \Y_WIDTH 2
    connect \A \out1
    connect \B \out2
    connect \Y { $n22 $n21 }
  end
  process $proc$/home/jamiespa/odin3-ws/odin3-2F3/tests/micro/verilog/micro/bm_if_collapse.v:69$9
    assign { $n15 $n14 } \out1
    assign { $n17 $n16 } \out2
    assign { $n13 $n12 } { $n22 $n21 }
    switch \a_in
      case 2'00
        switch $n20
          case 1'1
            assign { $n17 $n16 } 2'11
        end
      case 2'01
        assign { $n15 $n14 } 2'10
      case 2'10
        assign { $n15 $n14 } 2'01
      case 2'11
        assign { $n15 $n14 } 2'00
    end
    sync posedge \clock
      update \out { $n13 $n12 }
      update \out1 { $n15 $n14 }
      update \out2 { $n17 $n16 }
  end
end
