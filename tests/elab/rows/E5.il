# generated: elab-check --write
attribute \top 1
module \e5_div_mod_pow2
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 8 output 3 \y1
  wire width 8 output 4 \y2
  wire width 8 output 5 \y3
  wire width 8 output 6 \y4
  wire width 8 output 7 \y5
  wire $n59
  wire $n60
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  wire $n65
  wire $n66
  wire $n67
  wire $n68
  wire $n69
  wire $n70
  wire $n71
  wire $n72
  wire $n73
  wire $n74
  wire $n75
  wire $n76
  wire $n77
  wire $n78
  wire $n79
  wire $n80
  wire $n81
  wire $n82
  wire $n83
  wire $n84
  wire $n85
  wire $n86
  wire $n87
  wire $n88
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
  wire $n191
  wire $n192
  wire $n193
  wire $n194
  wire $n195
  wire $n196
  wire $n197
  wire $n198
  wire $n199
  wire $n200
  wire $n201
  wire $n202
  wire $n203
  wire $n204
  wire $n205
  wire $n206
  wire $n207
  wire $n208
  wire $n209
  wire $n210
  wire $n211
  wire $n212
  wire $n213
  wire $n214
  wire $n215
  wire $n216
  wire $n217
  wire $n218
  wire $n219
  wire $n220
  wire $n221
  wire $n222
  wire $n223
  wire $n224
  wire $n225
  wire $n226
  wire $n227
  wire $n228
  wire $n229
  wire $n230
  wire $n231
  wire $n232
  wire $n233
  wire $n234
  wire $n235
  wire $n236
  wire $n237
  wire $n238
  wire $n239
  wire $n240
  wire $n241
  wire $n242
  wire $n243
  wire $n244
  wire $n245
  wire $n246
  wire $n247
  wire $n248
  wire $n249
  wire $n250
  wire $n251
  wire $n252
  wire $n253
  wire $n254
  wire $n255
  wire $n256
  wire $n257
  wire $n258
  wire $n259
  wire $n260
  wire $n261
  wire $n262
  wire $n263
  wire $n264
  wire $n265
  wire $n266
  wire $n267
  wire $n268
  wire $n269
  wire $n270
  wire $n271
  wire $n272
  wire $n273
  wire $n274
  wire $n275
  wire $n276
  wire $n277
  wire $n278
  wire $n279
  wire $n280
  wire $n281
  wire $n282
  cell $div $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000100
    connect \Y { $n90 $n89 $n88 $n87 $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 }
  end
  cell $mod $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000100
    connect \Y { $n122 $n121 $n120 $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 $n99 $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 }
  end
  cell $div $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B 8'00001000
    connect \Y \y3
  end
  cell $mul $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B \b
    connect \Y { $n154 $n153 $n152 $n151 $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 $n142 $n141 $n140 $n139 $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 }
  end
  cell $div $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A { $n154 $n153 $n152 $n151 $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 $n142 $n141 $n140 $n139 $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 }
    connect \B 32'00000000000000000000000000000100
    connect \Y { $n186 $n185 $n184 $n183 $n182 $n181 $n180 $n179 $n178 $n177 $n176 $n175 $n174 $n173 $n172 $n171 $n170 $n169 $n168 $n167 $n166 $n165 $n164 $n163 $n162 $n161 $n160 $n159 $n158 $n157 $n156 $n155 }
  end
  cell $mod $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A { $n186 $n185 $n184 $n183 $n182 $n181 $n180 $n179 $n178 $n177 $n176 $n175 $n174 $n173 $n172 $n171 $n170 $n169 $n168 $n167 $n166 $n165 $n164 $n163 $n162 $n161 $n160 $n159 $n158 $n157 $n156 $n155 }
    connect \B 32'00000000000000000000000000000010
    connect \Y { $n218 $n217 $n216 $n215 $n214 $n213 $n212 $n211 $n210 $n209 $n208 $n207 $n206 $n205 $n204 $n203 $n202 $n201 $n200 $n199 $n198 $n197 $n196 $n195 $n194 $n193 $n192 $n191 $n190 $n189 $n188 $n187 }
  end
  cell $div $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \b
    connect \B 32'00000000000000000000000000000100
    connect \Y { $n250 $n249 $n248 $n247 $n246 $n245 $n244 $n243 $n242 $n241 $n240 $n239 $n238 $n237 $n236 $n235 $n234 $n233 $n232 $n231 $n230 $n229 $n228 $n227 $n226 $n225 $n224 $n223 $n222 $n221 $n220 $n219 }
  end
  cell $add $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B { $n250 $n249 $n248 $n247 $n246 $n245 $n244 $n243 $n242 $n241 $n240 $n239 $n238 $n237 $n236 $n235 $n234 $n233 $n232 $n231 $n230 $n229 $n228 $n227 $n226 $n225 $n224 $n223 $n222 $n221 $n220 $n219 }
    connect \Y { $n282 $n281 $n280 $n279 $n278 $n277 $n276 $n275 $n274 $n273 $n272 $n271 $n270 $n269 $n268 $n267 $n266 $n265 $n264 $n263 $n262 $n261 $n260 $n259 $n258 $n257 $n256 $n255 $n254 $n253 $n252 $n251 }
  end
  connect \y1 { $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 }
  connect \y2 { $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 }
  connect \y4 { $n194 $n193 $n192 $n191 $n190 $n189 $n188 $n187 }
  connect \y5 { $n258 $n257 $n256 $n255 $n254 $n253 $n252 $n251 }
end
