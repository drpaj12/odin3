# generated: elab-check --write
attribute \top 1
module \e7_power
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire width 16 output 4 \y1
  wire width 16 output 5 \y2
  wire width 16 output 6 \y3
  wire width 16 output 7 \y4
  wire width 16 output 8 \y5
  wire width 16 output 9 \y6
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
  wire $n151
  wire $n152
  wire $n153
  wire $n154
  wire $n155
  wire $n156
  wire $n157
  wire $n158
  wire $n159
  wire $n160
  wire $n161
  wire $n162
  wire $n163
  wire $n164
  wire $n165
  wire $n166
  wire $n167
  wire $n168
  wire $n169
  wire $n170
  wire $n171
  wire $n172
  wire $n173
  wire $n174
  wire $n175
  wire $n176
  wire $n177
  wire $n178
  wire $n179
  wire $n180
  wire $n181
  wire $n182
  wire $n183
  wire $n184
  wire $n185
  wire $n186
  wire $n187
  wire $n188
  wire $n189
  wire $n190
  cell $pow $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \b
    connect \Y \y2
  end
  cell $shl $c13
    parameter \A_SIGNED 1
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 32
    connect \A 32'00000000000000000000000000000001
    connect \B \b
    connect \Y { $n142 $n141 $n140 $n139 $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n120 $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 }
  end
  cell $pow $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B \b
    connect \Y { $n158 $n157 $n156 $n155 $n154 $n153 $n152 $n151 $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 }
  end
  cell $pow $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 16
    connect \A { $n158 $n157 $n156 $n155 $n154 $n153 $n152 $n151 $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 }
    connect \B \c
    connect \Y \y4
  end
  cell $neg $c16
    parameter \A_SIGNED 0
    parameter \A_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A { 12'000000000000 \a }
    connect \Y { $n174 $n173 $n172 $n171 $n170 $n169 $n168 $n167 $n166 $n165 $n164 $n163 $n162 $n161 $n160 $n159 }
  end
  cell $pow $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 16
    connect \A { $n174 $n173 $n172 $n171 $n170 $n169 $n168 $n167 $n166 $n165 $n164 $n163 $n162 $n161 $n160 $n159 }
    connect \B \b
    connect \Y \y5
  end
  cell $pow $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 16
    connect \A \b
    connect \B \c
    connect \Y { $n190 $n189 $n188 $n187 $n186 $n185 $n184 $n183 $n182 $n181 $n180 $n179 $n178 $n177 $n176 $n175 }
  end
  cell $mul $c19
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \a
    connect \B { $n190 $n189 $n188 $n187 $n186 $n185 $n184 $n183 $n182 $n181 $n180 $n179 $n178 $n177 $n176 $n175 }
    connect \Y \y6
  end
  connect \y1 16'0000000000001000
  connect \y3 { $n126 $n125 $n124 $n123 $n122 $n121 $n120 $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 }
end
