#!/usr/bin/env python
"""Example code of learning a large scale convnet from ILSVRC2012 dataset.

Prerequisite: To run this example, crop the center of ILSVRC2012 training and
validation images, scale them to 256x256 and convert them to RGB, and make
two lists of space-separated CSV whose first column is full path to image and
second column is zero-origin label (this format is same as that used by Caffe's
ImageDataLayer).

"""
import argparse
import json
import random

import numpy as np

import chainer
from chainer import dataset
from chainer import function_hooks
from chainer import training
from chainer.training import extensions
import chainerx

import dali_util

import alex
import googlenet
import googlenetbn
import nin
import resnet50
import resnext50

import chainer_compiler


class PreprocessedDataset(chainer.dataset.DatasetMixin):

    def __init__(self, path, root, mean, crop_size, random=True):
        self.base = chainer.datasets.LabeledImageDataset(path, root)
        self.mean = mean.astype(np.float32)
        self.crop_size = crop_size
        self.random = random

    def __len__(self):
        return len(self.base)

    def get_example(self, i):
        # It reads the i-th image/label pair and return a preprocessed image.
        # It applies following preprocesses:
        #     - Cropping (random or center rectangular)
        #     - Random flip
        #     - Scaling to [0, 1] value
        crop_size = self.crop_size

        image, label = self.base[i]
        _, h, w = image.shape

        if self.random:
            # Randomly crop a region and flip the image
            top = random.randint(0, h - crop_size - 1)
            left = random.randint(0, w - crop_size - 1)
            if random.randint(0, 1):
                image = image[:, :, ::-1]
        else:
            # Crop the center
            top = (h - crop_size) // 2
            left = (w - crop_size) // 2
        bottom = top + crop_size
        right = left + crop_size

        image = image[:, top:bottom, left:right]
        image -= self.mean[:, top:bottom, left:right]
        image *= (1.0 / 255.0)  # Scale to [0, 1]
        return image, label


def main():
    archs = {
        'alex': alex.Alex,
        'alex_fp16': alex.AlexFp16,
        'googlenet': googlenet.GoogLeNet,
        'googlenetbn': googlenetbn.GoogLeNetBN,
        'googlenetbn_fp16': googlenetbn.GoogLeNetBNFp16,
        'nin': nin.NIN,
        'resnet50': resnet50.ResNet50,
        'resnext50': resnext50.ResNeXt50,
    }

    parser = argparse.ArgumentParser(
        description='Learning convnet from ILSVRC2012 dataset')
    parser.add_argument('train', help='Path to training image-label list file')
    parser.add_argument('val', help='Path to validation image-label list file')
    parser.add_argument('--arch', '-a', choices=archs.keys(), default='nin',
                        help='Convnet architecture')
    parser.add_argument('--batchsize', '-B', type=int, default=32,
                        help='Learning minibatch size')
    parser.add_argument('--epoch', '-E', type=int, default=10,
                        help='Number of epochs to train')
    parser.add_argument('--iterations', '-I', type=int, default=0,
                        help='Number of iterations to train')
    parser.add_argument('--device', '-d', type=str, default='-1',
                        help='Device specifier. Either ChainerX device '
                        'specifier or an integer. If non-negative integer, '
                        'CuPy arrays with specified device id are used. If '
                        'negative integer, NumPy arrays are used')
    parser.add_argument('--initmodel',
                        help='Initialize the model from given file')
    parser.add_argument('--loaderjob', '-j', type=int,
                        help='Number of parallel data loading processes')
    parser.add_argument('--mean', '-m', default='mean.npy',
                        help='Mean file (computed by compute_mean.py)')
    parser.add_argument('--resume', '-r', default='',
                        help='Initialize the trainer from given file')
    parser.add_argument('--out', '-o', default='result',
                        help='Output directory')
    parser.add_argument('--root', '-R', default='.',
                        help='Root directory path of image files')
    parser.add_argument('--val_batchsize', '-b', type=int, default=250,
                        help='Validation minibatch size')
    parser.add_argument('--test', action='store_true')
    parser.set_defaults(test=False)
    parser.add_argument('--dali', action='store_true')
    parser.set_defaults(dali=False)
    group = parser.add_argument_group('deprecated arguments')
    group.add_argument('--gpu', '-g', dest='device',
                       type=int, nargs='?', const=0,
                       help='GPU ID (negative value indicates CPU)')
    parser.add_argument('--compile', action='store_true',
                        help='Compile the model')
    parser.add_argument('--dump_onnx', action='store_true',
                        help='Dump ONNX model after optimization')
    args = parser.parse_args()

    chainer.config.autotune = True
    chainer.config.cudnn_fast_batch_normalization = True

    device = chainer.get_device(args.device)

    print('Device: {}'.format(device))
    print('# Minibatch-size: {}'.format(args.batchsize))
    if args.iterations:
        print('# iterations: {}'.format(args.iterations))
    else:
        print('# epoch: {}'.format(args.epoch))
    print('')

    # Initialize the model to train
    model = archs[args.arch]()
    if args.initmodel:
        print('Load model from {}'.format(args.initmodel))
        chainer.serializers.load_npz(args.initmodel, model)
    insize = model.insize
    if args.compile:
        model = chainer_compiler.compile(model, dump_onnx=args.dump_onnx)
    model.to_device(device)
    device.use()

    # Load the mean file
    mean = np.load(args.mean)
    if args.dali:
        if not dali_util._dali_available:
            raise RuntimeError('DALI seems not available on your system.')
        num_threads = args.loaderjob
        if num_threads is None or num_threads <= 0:
            num_threads = 1
        ch_mean = list(np.average(mean, axis=(1, 2)))
        ch_std = [255.0, 255.0, 255.0]
        # Setup DALI pipelines
        train_pipe = dali_util.DaliPipelineTrain(
            args.train, args.root, insize, args.batchsize,
            num_threads, args.gpu, True, mean=ch_mean, std=ch_std)
        val_pipe = dali_util.DaliPipelineVal(
            args.val, args.root, insize, args.val_batchsize,
            num_threads, args.gpu, False, mean=ch_mean, std=ch_std)
        train_iter = chainer.iterators.DaliIterator(train_pipe)
        val_iter = chainer.iterators.DaliIterator(val_pipe, repeat=False)
        # converter = dali_converter
        converter = dali_util.DaliConverter(mean=mean, crop_size=insize)
    else:
        # Load the dataset files
        train = PreprocessedDataset(args.train, args.root, mean, insize)
        val = PreprocessedDataset(args.val, args.root, mean, insize,
                                  False)
        # These iterators load the images with subprocesses running in parallel
        # to the training/validation.
        train_iter = chainer.iterators.MultiprocessIterator(
            train, args.batchsize, n_processes=args.loaderjob)
        val_iter = chainer.iterators.MultiprocessIterator(
            val, args.val_batchsize, repeat=False, n_processes=args.loaderjob)
        converter = dataset.concat_examples

    # Set up an optimizer
    optimizer = chainer.optimizers.MomentumSGD(lr=0.01, momentum=0.9)
    optimizer.setup(model)

    # Set up a trainer
    updater = training.updaters.StandardUpdater(
        train_iter, optimizer, converter=converter, device=device)
    if args.iterations:
        stop_trigger = (args.iterations, 'iteration')
    else:
        stop_trigger = (args.epoch, 'epoch')
    trainer = training.Trainer(updater, stop_trigger, args.out)

    val_interval = (1 if args.test else 100000), 'iteration'
    log_interval = ((1 if args.test else 10 if args.iterations else 1000),
                    'iteration')

    trainer.extend(extensions.Evaluator(val_iter, model, converter=converter,
                                        device=device), trigger=val_interval)
    # TODO(sonots): Temporarily disabled for chainerx. Fix it.
    if device.xp is not chainerx:
        trainer.extend(extensions.DumpGraph('main/loss'))
    trainer.extend(extensions.snapshot(), trigger=val_interval)
    trainer.extend(extensions.snapshot_object(
        model, 'model_iter_{.updater.iteration}'), trigger=val_interval)
    # Be careful to pass the interval directly to LogReport
    # (it determines when to emit log rather than when to read observations)
    trainer.extend(extensions.LogReport(trigger=log_interval))
    trainer.extend(extensions.observe_lr(), trigger=log_interval)
    trainer.extend(extensions.PrintReport([
        'epoch', 'iteration', 'main/loss', 'validation/main/loss',
        'main/accuracy', 'validation/main/accuracy', 'lr'
    ]), trigger=log_interval)
    trainer.extend(extensions.ProgressBar(update_interval=10))

    if args.resume:
        chainer.serializers.load_npz(args.resume, trainer)

    cuda_hook = function_hooks.CUDAProfileHook()
    with cuda_hook:
        trainer.run()

    with open('%s/log' % args.out) as f:
        logs = json.load(f)
    elapsed_times = []
    for prev, cur in zip(logs, logs[1:]):
        iters = cur['iteration'] - prev['iteration']
        elapsed = cur['elapsed_time'] - prev['elapsed_time']
        elapsed_times.append(elapsed / iters)
    sec_per_iter = sum(elapsed_times) / len(elapsed_times)
    print(sec_per_iter * 1000, 'msec/iter')
    print(args.batchsize / sec_per_iter, 'images/sec')


if __name__ == '__main__':
    main()
