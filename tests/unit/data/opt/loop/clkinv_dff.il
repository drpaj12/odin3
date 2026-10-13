module \m
  wire input 1 \c
  wire input 2 \d
  attribute \init 1'1
  wire output 3 \q
  wire \n
  cell $not \inv
    connect \A \c
    connect \Y \n
  end
  cell $dff \ff
    connect \CLK \n
    connect \D \d
    connect \Q \q
  end
end
