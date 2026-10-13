module \m
  wire input 1 \c
  wire input 2 \d
  attribute \init 1'1
  wire output 3 \q
  wire \

  cell $not \inv
    connect \A \c
    connect \Y \

  end
  cell $dff \ff
    connect \CLK \

    connect \D \d
    connect \Q \q
  end
end
