# generated: elab-check --write
attribute \top 1
module \n12_multidim
  wire width 2 input 1 \i
  wire width 2 input 2 \j
  wire width 8 output 3 \y
  wire output 4 \z
  wire $n16
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
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
  cell $mul $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 2
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \i
    connect \B 32'00000000000000000000000000000010
    connect \Y { $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 $n18 $n17 $n16 }
  end
  cell $add $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 32
    connect \A { $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 $n18 $n17 $n16 }
    connect \B \j [0]
    connect \Y { $n79 $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 $n48 }
  end
  cell $sub $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 32
    connect \A 32'00000000000000000000000000000001
    connect \B \i [0]
    connect \Y { $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 $n99 $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 $n88 }
  end
  cell $mul $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A { $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 $n99 $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 $n88 }
    connect \B 32'00000000000000000000000000000100
    connect \Y { $n151 $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 $n142 $n141 $n140 $n139 $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n120 }
  end
  cell $sub $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 32
    connect \A 32'00000000000000000000000000000001
    connect \B \j [0]
    connect \Y { $n183 $n182 $n181 $n180 $n179 $n178 $n177 $n176 $n175 $n174 $n173 $n172 $n171 $n170 $n169 $n168 $n167 $n166 $n165 $n164 $n163 $n162 $n161 $n160 $n159 $n158 $n157 $n156 $n155 $n154 $n153 $n152 }
  end
  cell $mul $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A { $n183 $n182 $n181 $n180 $n179 $n178 $n177 $n176 $n175 $n174 $n173 $n172 $n171 $n170 $n169 $n168 $n167 $n166 $n165 $n164 $n163 $n162 $n161 $n160 $n159 $n158 $n157 $n156 $n155 $n154 $n153 $n152 }
    connect \B 32'00000000000000000000000000000010
    connect \Y { $n215 $n214 $n213 $n212 $n211 $n210 $n209 $n208 $n207 $n206 $n205 $n204 $n203 $n202 $n201 $n200 $n199 $n198 $n197 $n196 $n195 $n194 $n193 $n192 $n191 $n190 $n189 $n188 $n187 $n186 $n185 $n184 }
  end
  cell $add $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A { $n151 $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 $n142 $n141 $n140 $n139 $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n120 }
    connect \B { $n215 $n214 $n213 $n212 $n211 $n210 $n209 $n208 $n207 $n206 $n205 $n204 $n203 $n202 $n201 $n200 $n199 $n198 $n197 $n196 $n195 $n194 $n193 $n192 $n191 $n190 $n189 $n188 $n187 $n186 $n185 $n184 }
    connect \Y { $n247 $n246 $n245 $n244 $n243 $n242 $n241 $n240 $n239 $n238 $n237 $n236 $n235 $n234 $n233 $n232 $n231 $n230 $n229 $n228 $n227 $n226 $n225 $n224 $n223 $n222 $n221 $n220 $n219 $n218 $n217 $n216 }
  end
  cell $add $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 32
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A { $n247 $n246 $n245 $n244 $n243 $n242 $n241 $n240 $n239 $n238 $n237 $n236 $n235 $n234 $n233 $n232 $n231 $n230 $n229 $n228 $n227 $n226 $n225 $n224 $n223 $n222 $n221 $n220 $n219 $n218 $n217 $n216 }
    connect \B 32'00000000000000000000000000000000
    connect \Y { $n279 $n278 $n277 $n276 $n275 $n274 $n273 $n272 $n271 $n270 $n269 $n268 $n267 $n266 $n265 $n264 $n263 $n262 $n261 $n260 $n259 $n258 $n257 $n256 $n255 $n254 $n253 $n252 $n251 $n250 $n249 $n248 }
  end
  cell $mem $c16
    parameter \MEMID "\\m2"
    parameter \SIZE 8
    parameter \OFFSET 0
    parameter \ABITS 32
    parameter \WIDTH 8
    parameter \RD_PORTS 1
    parameter \WR_PORTS 0
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'0
    parameter \WR_CLK_POLARITY 1'0
    parameter \INIT 0'
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR { $n79 $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 $n48 }
    connect \RD_DATA { $n87 $n86 $n85 $n84 $n83 $n82 $n81 $n80 }
    connect \WR_CLK { }
    connect \WR_EN { }
    connect \WR_ADDR { }
    connect \WR_DATA { }
  end
  cell $mem $c17
    parameter \MEMID "\\m3"
    parameter \SIZE 8
    parameter \OFFSET 0
    parameter \ABITS 32
    parameter \WIDTH 1
    parameter \RD_PORTS 1
    parameter \WR_PORTS 0
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'0
    parameter \WR_CLK_POLARITY 1'0
    parameter \INIT 0'
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR { $n279 $n278 $n277 $n276 $n275 $n274 $n273 $n272 $n271 $n270 $n269 $n268 $n267 $n266 $n265 $n264 $n263 $n262 $n261 $n260 $n259 $n258 $n257 $n256 $n255 $n254 $n253 $n252 $n251 $n250 $n249 $n248 }
    connect \RD_DATA $n280
    connect \WR_CLK { }
    connect \WR_EN { }
    connect \WR_ADDR { }
    connect \WR_DATA { }
  end
  connect \y { $n87 $n86 $n85 $n84 $n83 $n82 $n81 $n80 }
  connect \z $n280
end
