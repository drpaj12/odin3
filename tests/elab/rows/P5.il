# generated: elab-check --write
attribute \top 1
module \p5_override
  wire width 8 input 1 \a
  wire width 8 output 2 \y1
  wire width 8 output 3 \y2
  wire width 8 output 4 \y3
  wire width 8 output 5 \y4
  attribute \parameter:W 32'00000000000000000000000000001000
  attribute \parameter:D 32'00000000000000000000000000000010
  cell \p5_leaf$W=8 \u1
    connect \a \a
    connect \y \y1
  end
  attribute \parameter:W 32'00000000000000000000000000001000
  attribute \parameter:D 32'00000000000000000000000000000011
  cell \p5_leaf$W=8$D=3 \u2
    connect \a \a
    connect \y \y2
  end
  attribute \parameter:W 32'00000000000000000000000000001000
  attribute \parameter:D 32'00000000000000000000000000000010
  cell \p5_leaf$W=8 \u3
    connect \a \a
    connect \y \y3
  end
  attribute \parameter:W 32'00000000000000000000000000001000
  attribute \parameter:D 32'00000000000000000000000000000010
  cell \p5_leaf$W=8 \u4
    connect \a \a
    connect \y \y4
  end
end
module \p5_leaf$W=8
  wire width 8 input 1 \a
  wire width 8 output 2 \y
  connect \y \a
end
module \p5_leaf$W=8$D=3
  wire width 8 input 1 \a
  wire width 8 output 2 \y
  connect \y \a
end
