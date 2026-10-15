attribute \top 1
module \sum_chain
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire width 8 output 4 \y
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
  wire $n113
  wire $n114
  wire $n115
  wire $n116
  wire $n117
  wire $n118
  wire $n119
  wire $n120
  wire $n121
  wire $n122
  wire $n123
  wire $n124
  wire $n125
  wire $n126
  wire $n127
  wire $n128
  wire $n129
  wire $n130
  wire $n131
  wire $n132
  wire $n133
  wire $n134
  wire $n135
  wire $n136
  cell $add $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 5
    connect \A \a
    connect \B \b
    connect \Y { $n123 $n122 $n121 $n120 $n119 }
  end
  cell $add $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 5
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 6
    connect \A { $n123 $n122 $n121 $n120 $n119 }
    connect \B \c
    connect \Y { $n129 $n128 $n127 $n126 $n125 $n124 }
  end
  cell $add $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 6
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 7
    connect \A { $n129 $n128 $n127 $n126 $n125 $n124 }
    connect \B 1'1
    connect \Y { $n136 $n135 $n134 $n133 $n132 $n131 $n130 }
  end
  connect \y { 1'0 $n136 $n135 $n134 $n133 $n132 $n131 $n130 }
end
