attribute \top 1
module \t
  wire input 1 \c
  wire width 3 input 2 \ra
  wire width 8 input 3 \d
  wire width 8 output 4 \q
  wire width 8 \m[0]
  wire width 8 \m[1]
  wire width 8 \m[2]
  wire width 8 \m[3]
  wire width 8 \m[4]
  wire width 8 \m[5]
  wire width 8 \m[6]
  wire width 8 \m[7]
  wire $n85
  wire $n86
  wire $n87
  wire $n88
  wire $n89
  wire $n90
  wire $n91
  wire $n92
  wire $n112
  wire $n121
  wire $n122
  wire $n123
  wire $n124
  wire $n125
  wire $n126
  wire $n127
  attribute \full_case 1
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \ra
    connect \B 3'111
    connect \Y $n112
  end
  attribute \full_case 1
  cell $eq $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \ra
    connect \B 3'110
    connect \Y $n121
  end
  attribute \full_case 1
  cell $eq $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \ra
    connect \B 3'101
    connect \Y $n122
  end
  attribute \full_case 1
  cell $eq $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \ra
    connect \B 3'100
    connect \Y $n123
  end
  attribute \full_case 1
  cell $eq $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \ra
    connect \B 3'011
    connect \Y $n124
  end
  attribute \full_case 1
  cell $eq $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \ra
    connect \B 3'010
    connect \Y $n125
  end
  attribute \full_case 1
  cell $eq $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \ra
    connect \B 3'001
    connect \Y $n126
  end
  attribute \full_case 1
  cell $eq $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \ra
    connect \B 3'000
    connect \Y $n127
  end
  attribute \full_case 1
  cell $pmux $c18
    parameter \WIDTH 8
    parameter \S_WIDTH 8
    connect \A 8'xxxxxxxx
    connect \B { \m[0] \m[1] \m[2] \m[3] \m[4] \m[5] \m[6] \m[7] }
    connect \S { $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n112 }
    connect \Y { $n92 $n91 $n90 $n89 $n88 $n87 $n86 $n85 }
  end
  cell $dff $c19
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    connect \CLK \c
    connect \D \d
    connect \Q \m[3]
  end
  connect \q { $n92 $n91 $n90 $n89 $n88 $n87 $n86 $n85 }
end
