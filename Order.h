#ifndef ORDER_H
#define ORDER_H

struct Order {
    double price;
    int size;

    // Constructor to initialize Order
    Order(double price_, int size_) : price(price_), size(size_) {}
};

#endif // ORDER_H