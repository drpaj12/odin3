# generated: elab-check --write
attribute \top 1
module \e10_shifts
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 8 input 3 signed \s
  wire width 3 input 4 \n
  wire width 8 output 5 \y1
  wire width 8 output 6 \y2
  wire width 8 output 7 \y3
  wire width 8 output 8 \y4
  wire width 8 output 9 \y5
  wire width 8 output 10 \y6
  wire width 8 output 11 \y7
  wire width 8 output 12 \y8
  wire width 8 output 13 \y9
  wire output 14 \y10
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
  wire $n137
  wire $n138
  wire $n139
  wire $n140
  wire $n141
  wire $n142
  wire $n143
  wire $n144
  wire $n145
  wire $n146
  wire $n147
  wire $n148
  wire $n149
  wire $n150
  cell $shl $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B 32'00000000000000000000000000000010
    connect \Y \y1
  end
  cell $shr $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B 32'00000000000000000000000000000010
    connect \Y \y2
  end
  cell $shl $c19
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B 32'00000000000000000000000000000010
    connect \Y \y3
  end
  cell $sshr $c20
    parameter \A_SIGNED 1
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A \s
    connect \B 32'00000000000000000000000000000010
    connect \Y \y4
  end
  cell $shl $c21
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \n
    connect \Y \y5
  end
  cell $shr $c22
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \n
    connect \Y \y6
  end
  cell $sshr $c23
    parameter \A_SIGNED 1
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 8
    connect \A \s
    connect \B \n
    connect \Y \y7
  end
  cell $add $c24
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y { $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 }
  end
  cell $shl $c25
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A { $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 }
    connect \B 32'00000000000000000000000000000001
    connect \Y \y8
  end
  cell $add $c26
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \n
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n142 $n141 $n140 $n139 $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n120 $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 }
  end
  cell $shl $c27
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B { $n142 $n141 $n140 $n139 $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n120 $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 }
    connect \Y \y9
  end
  cell $shl $c28
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 }
  end
  cell $lt $c29
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 1
    connect \A { $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 }
    connect \B \b
    connect \Y \y10
  end
end
