attribute \top 1
module \to_signed_sx_add
  wire input 1 \clk
  wire width 9 input 2 \a
  wire width 9 input 3 \b
  wire width 11 output 4 \y
  wire width 9 \ra
  wire width 9 \rb
  wire $n89
  wire $n90
  wire $n91
  wire $n92
  wire $n93
  wire $n94
  wire $n95
  wire $n96
  wire $n97
  wire $n98
  wire $n100
  wire $n101
  wire $n102
  wire $n103
  wire $n104
  wire $n105
  wire $n106
  wire $n107
  wire $n108
  wire $n109
  cell $dff $c8
    parameter \WIDTH 9
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \a
    connect \Q \ra
  end
  cell $dff $c9
    parameter \WIDTH 9
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \b
    connect \Q \rb
  end
  cell $add $c10
    parameter \A_SIGNED 1
    parameter \B_SIGNED 1
    parameter \A_WIDTH 9
    parameter \B_WIDTH 9
    parameter \Y_WIDTH 10
    connect \A \ra
    connect \B \rb
    connect \Y { $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 }
  end
  cell $dff $c12
    parameter \WIDTH 10
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 }
    connect \Q { $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 }
  end
  connect \y { $n109 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 }
end
