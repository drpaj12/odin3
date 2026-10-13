# generated: elab-check --write
attribute \top 1
module \f1_for
  wire width 8 input 1 \a
  wire width 8 output 2 \y1
  wire width 8 output 3 \y2
  wire width 8 output 4 \y3
  wire width 32 signed \i
  wire width 32 signed \k
  wire width 4 \j
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
  process $proc$F1_for_loops.v:22$7
    assign { $n116 $n115 $n114 $n113 $n112 $n111 $n110 $n109 } \a
    assign $n149 \a [7]
    assign $n150 \a [6]
    assign $n151 \a [5]
    assign $n152 \a [4]
    assign $n153 \a [3]
    assign $n154 \a [2]
    assign $n155 \a [1]
    assign $n156 \a [0]
    assign { $n148 $n147 $n146 $n145 $n144 $n143 $n142 $n141 $n140 $n139 $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n120 $n119 $n118 $n117 } 32'00000000000000000000000000001000
    sync always
      update { $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 } { $n156 $n155 $n154 $n153 $n152 $n151 $n150 $n149 }
  end
  process $proc$F1_for_loops.v:5$8
    assign $n159 \a [7]
    assign $n160 \a [6]
    assign $n161 \a [5]
    assign $n162 \a [4]
    assign $n163 \a [3]
    assign $n164 \a [2]
    assign $n165 \a [1]
    assign $n166 \a [0]
    sync always
      update \y1 { $n166 $n165 $n164 $n163 $n162 $n161 $n160 $n159 }
  end
  process $proc$F1_for_loops.v:8$9
    assign { $n173 $n171 $n169 $n167 } 4'0000
    assign $n168 \a [0]
    assign $n170 \a [2]
    assign $n172 \a [4]
    assign $n174 \a [6]
    sync always
      update \y2 { $n174 $n173 $n172 $n171 $n170 $n169 $n168 $n167 }
  end
  connect \y3 { $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 }
end
