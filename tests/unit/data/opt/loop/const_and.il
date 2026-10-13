module \m
  wire input 1 \a
  wire output 2 \y
  cell $and \c
    connect \A \a
    connect \B 1'0
    connect \Y \y
  end
end
