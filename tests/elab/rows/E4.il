# generated: elab-check --write
attribute \top 1
module \e4_arith
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 8 input 3 \c
  wire width 8 output 4 \y1
  wire width 8 output 5 \y2
  wire width 8 output 6 \y3
  wire width 8 output 7 \y4
  wire width 8 output 8 \y5
  wire width 8 output 9 \y6
  wire width 8 output 10 \y7
  wire width 8 output 11 \y8
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
  wire $n99
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
  wire $n110
  wire $n111
  wire $n112
  cell $add $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y \y1
  end
  cell $sub $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y \y2
  end
  cell $mul $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y \y3
  end
  cell $neg $c15
    parameter \A_SIGNED 0
    parameter \A_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \Y \y4
  end
  cell $mul $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \b
    connect \B \c
    connect \Y { $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 }
  end
  cell $add $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B { $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 }
    connect \Y \y6
  end
  cell $sub $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y { $n104 $n103 $n102 $n101 $n100 $n99 $n98 $n97 }
  end
  cell $sub $c19
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A { $n104 $n103 $n102 $n101 $n100 $n99 $n98 $n97 }
    connect \B \c
    connect \Y \y7
  end
  cell $neg $c20
    parameter \A_SIGNED 0
    parameter \A_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \Y { $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 }
  end
  cell $mul $c21
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A { $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 }
    connect \B \b
    connect \Y \y8
  end
  connect \y5 \a
end
